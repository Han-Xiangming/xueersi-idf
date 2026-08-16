/*
 * Hardware layer: I2S audio output driving a MAX98357 stereo Class-D DAC.
 *
 * Streams decoded MP3 PCM over I2S (BCLK/LRC/DIN); the MAX98357 derives its
 * own master clock from BCLK, so no MCLK is wired.
 *
 * Direct-write architecture: the MP3 player task applies the DSP chain and
 * writes PCM straight to the I2S DMA (no ring buffer / feed task). The DMA
 * paces the decoder by back-pressure; the channel is enabled by the first
 * write of a speaker session and parked (BCLK stopped, amp powered down)
 * while idle or on the Bluetooth route.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Audio output route: a single, explicit either/or switch. Exactly ONE
 * destination is active at any time and only hw_audio_set_route() changes it.
 * Bluetooth connection state does NOT touch the route — the caller (UI) must
 * flip it explicitly when it wants Bluetooth output. This keeps playback
 * deterministic: a speaker session never gets silently hijacked by a Bluetooth
 * link coming up. */
typedef enum {
    AUDIO_ROUTE_SPEAKER,   /* local MAX98357 I2S speaker (default) */
    AUDIO_ROUTE_BT,        /* Bluetooth A2DP sink (headphones / BT speaker) */
} audio_route_t;

/* Initialize the I2S peripheral and the MAX98357 DAC. Leaves the route at its
 * default (SPEAKER) and the channel parked. */
void hw_audio_init(void);

/* Explicitly select the active output route. The writer streams to exactly
 * this destination. Switching away from the speaker parks the I2S channel
 * (the amp powers down) so it goes truly silent; switching back resumes it.
 * This is the ONLY way the route changes. */
void hw_audio_set_route(audio_route_t route);

/* Current active output route. */
audio_route_t hw_audio_get_route(void);

/* Volume (0..100 %) of the ACTIVE route: while a Bluetooth sink is linked
 * and BT output is on, the BT volume is adjusted; otherwise the speaker
 * volume. The two routes keep independent settings (see the route-specific
 * accessors below). */
void hw_audio_set_volume(uint8_t volume_pct);
uint8_t hw_audio_get_volume(void);

/* Route-specific volumes (0..100 %), for NVS persistence / restore. */
void hw_audio_set_speaker_volume(uint8_t volume_pct);
uint8_t hw_audio_get_speaker_volume(void);
void hw_audio_set_bt_volume(uint8_t volume_pct);
uint8_t hw_audio_get_bt_volume(void);

/* AVRCP absolute volume (0..127, full remote scale) with equal ~0.32 dB
 * steps, sharing the same gain table as set_volume; always writes the BT
 * route (the remote is a Bluetooth peer) and updates the percent view. */
void hw_audio_set_avrc_volume(uint8_t volume_0_127);

/* Per-track loudness gain in dB (ReplayGain 2.0, from the file's ID3 tags
 * written by tools like loudgain). Applied before the master volume on the
 * active route with ~5 ms smoothing; 0 dB = flat. Call once at each track
 * start (untagged tracks: 0 dB). */
void hw_audio_set_track_gain_db(float gain_db);

/* Reconfigure the I2S sample rate (e.g. to match an MP3 file's rate).
 * Applied immediately from the calling task; a running channel is parked
 * for the reconfig and re-enabled by the next PCM write, so the new clock
 * is always in place before the first data of a track. */
void hw_audio_set_sample_rate(uint32_t sample_rate_hz);

/* Mark/unmark the MP3 player as the owner of the I2S bus. Claiming only
 * arms the pipeline (the channel is enabled by the first PCM write);
 * releasing parks the channel immediately (BCLK stops, amp powers down).
 * Safe to call from any task, including while a write is in flight. */
void hw_audio_set_player_active(bool active);

/* Result of a PCM write, so the caller can distinguish "streamed" from
 * "the pipeline is wedged" (DMA not consuming) vs "playback was
 * deactivated mid-write" (pause/stop — not an error). */
typedef enum {
    AUDIO_WRITE_OK = 0,       /* streamed to the I2S DMA (or BT) */
    AUDIO_WRITE_STALLED,      /* I2S write failed / timed out (wedged) */
    AUDIO_WRITE_ABANDONED,    /* player deactivated mid-write: not an error */
} audio_write_result_t;

/* Stream raw 16-bit stereo PCM (L,R interleaved). `frames` = number of
 * L/R pairs. Used by the MP3 player to output decoded audio. Samples are
 * filtered in place (per-track ReplayGain + speaker-protection high-pass +
 * loudness shelf + volume + limiter, see the driver docs) before a bounded
 * direct write to the I2S DMA, which paces the caller by back-pressure. */
audio_write_result_t hw_audio_write_pcm(int16_t *stereo_frames, size_t frames);

/* True only after the I2S channel and the IO mutex are up. Callers must
 * NOT start playback before this returns true. */
bool hw_audio_is_ready(void);