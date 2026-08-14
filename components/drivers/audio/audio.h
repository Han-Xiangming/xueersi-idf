/*
 * Hardware layer: I2S audio output driving a MAX98357 mono Class-D DAC.
 *
 * Streams decoded MP3 PCM over I2S (BCLK/LRC/DIN); the MAX98357 derives its
 * own master clock from BCLK, so no MCLK is wired.
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
 * default (SPEAKER). */
void hw_audio_init(void);

/* Explicitly select the active output route. The writer streams to exactly
 * this destination. Switching away from the speaker parks the I2S feed (the
 * amp powers down) so it goes truly silent instead of starving its ring;
 * switching back resumes it. This is the ONLY way the route changes. */
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

/* Reconfigure the I2S sample rate (e.g. to match an MP3 file's rate). */
void hw_audio_set_sample_rate(uint32_t sample_rate_hz);

/* Mark/unmark the MP3 player as the owner of the I2S bus. */
void hw_audio_set_player_active(bool active);

/* Discard all PCM currently queued in the ring (leftovers from the previous
 * track) without parking the channel. Call before starting a new track so the
 * stream opens cleanly at the new sample rate. */
void hw_audio_flush(void);

/* Stream raw 16-bit stereo PCM (L,R interleaved). `frames` = number of
 * L/R pairs. Used by the MP3 player to output decoded audio. Samples are
 * filtered in place by the speaker-protection high-pass before enqueueing. */
void hw_audio_write_pcm(int16_t *stereo_frames, size_t frames);

/* True only after I2S, PCM ring and the feed task are all up. Callers must
 * NOT start playback before this returns true (prevents decode deadlock when
 * the feed task is missing). */
bool hw_audio_is_ready(void);
