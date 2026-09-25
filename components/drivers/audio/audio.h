/*
 * Hardware layer: I2S audio output driving a MAX98357 stereo Class-D DAC,
 * via an ESP-IDF i2s_std channel + a self-contained DMA writer task (codec-less).
 *
 * Streams decoded MP3 PCM over I2S (BCLK/LRC/DIN); the MAX98357 derives its
 * own master clock from BCLK, so no MCLK is wired.
 *
 * The I2S clock is FIXED for the whole session; decoded PCM is resampled to it
 * before the DSP. The i2s_std channel is enabled ONCE at init and the DMA writer
 * task only pause/resumes it (route switch to Bluetooth, end-of-playback park) —
 * never a runtime disable/re-enable of the out-link, so the ESP32 DMA wedge
 * cannot recur. A small ring buffer sits between the DSP and the writer task.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

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

/* Initialize the i2s_std I2S output and the MAX98357 DAC (codec-less).
 * Enables the TX channel at the fixed I2S rate and starts the DMA writer task,
 * which is left RUNNING. */
void hw_audio_init(void);

/* Explicitly select the active output route. The writer streams to exactly
 * this destination. Switching away from the speaker PAUSES the DMA writer task
 * (channel disabled, BCLK stops, amp powers down); switching back RESUMES it.
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

/* Global master gain in dB (user preamp, -12..+12, 0 = flat): a whole-signal
 * offset applied to BOTH routes right after the per-track gain and before
 * the master volume, so it works like a preamp knob — quiet sources can be
 * pushed past what the volume knob reaches at 100%, hot ones tamed. Smoothed
 * like the volume, so a change never clicks. Persisted by the UI. */
void hw_audio_set_master_gain_db(float gain_db);
float hw_audio_get_master_gain_db(void);

/* Declare the DECODER's native sample rate for the current track (e.g. an
 * MP3 file's rate). The I2S channel itself runs at ONE fixed rate (set once at
 * init); decoded PCM is resampled to it inside hw_audio_write_pcm, so a rate
 * change NEVER disables/rebuilds the channel — this removes the in-playback
 * out-link stop/start that wedged the ESP32 DMA on mixed-rate playlists.
 * Call once per track (its first decoded frame). A no-op when the decoder rate
 * already matches the fixed I2S rate. */
void hw_audio_set_sample_rate(uint32_t sample_rate_hz);

/* Mark/unmark the MP3 player as the owner of the I2S bus. Claiming resets the
 * DSP and RESUMES the DMA writer task (it may have been paused by a route
 * switch or by hw_audio_park() at the previous track's end). Releasing does
 * NOT pause the channel: it keeps clocking auto_clear digital silence so
 * pause/resume and track switches need no out-link stop/start (see audio.c).
 * Safe to call from any task, including while a write is in flight. */
void hw_audio_set_player_active(bool active);

/* Pause the DMA writer task (channel disabled, BCLK stops, MAX98357 powers
 * down). Call ONLY when playback is finished for good — i.e. when the decode
 * loop exits — never on pause or between tracks. The writer is resumed by the
 * next hw_audio_set_player_active(true). */
void hw_audio_park(void);

/* Drop what the previous pass left queued in the output pipeline and reset the
 * underrun bookkeeping, so the next PCM write starts a clean pass. Used at
 * repeat-one seams before the inter-pass pause.
 *  - Bluetooth: really flushes the stale PCM ring tail.
 *  - Speaker (I2S): asks the DMA writer task to drain the PCM ring and
 *    disable/re-enable the i2s_std channel, so the descriptor queue is reset
 *    and the next write starts from an empty DMA (auto_clear alone does NOT
 *    reset the queue, which is why a bare no-op here let the queue desync and
 *    wedge on repeat).
 * Call from the task that owns PCM writes. */
void hw_audio_pipeline_flush(void);

/* Hard track switch: like hw_audio_pipeline_flush() but the writer ALSO
 * overwrites the entire DMA descriptor ring with silence, so any previous
 * track's PCM still sitting in descriptors ahead of the DMA pointer is
 * discarded (disable/enable alone does not clear them). Use this on a real
 * track switch (stop->next, manual next/prev) where residual audio must not
 * bleed into the next song. Synchronous: blocks until the writer is done. */
void hw_audio_pipeline_switch(void);

/* Result of a PCM write, so the caller can distinguish "streamed" from
 * "the pipeline is wedged" (DMA not consuming) vs "playback was
 * deactivated mid-write" (pause/stop — not an error). */
typedef enum {
    AUDIO_WRITE_OK = 0,       /* streamed into the PCM ring (or BT) */
    AUDIO_WRITE_STALLED,      /* ring full / DMA not consuming (wedged) */
    AUDIO_WRITE_ABANDONED,    /* player deactivated mid-write: not an error */
} audio_write_result_t;

/* Stream raw 16-bit stereo PCM (L,R interleaved). `frames` = number of
 * L/R pairs. Used by the MP3 player to output decoded audio. Samples are
 * filtered in place (per-track ReplayGain + user master gain + speaker-
 * protection high-pass + loudness shelf + volume + limiter, see the driver
 * docs) then pushed into the PCM ring buffer via a bounded xRingbufferSend,
 * which paces the caller by back-pressure; the DMA writer task feeds the I2S
 * channel independently. */
audio_write_result_t hw_audio_write_pcm(int16_t *stereo_frames, size_t frames);

/* True only after the I2S channel and the DMA writer task are up. Callers
 * must NOT start playback before this returns true. */
bool hw_audio_is_ready(void);

/* True while the MP3 player owns the I2S bus AND the channel is actually
 * clocking data to the DAC (i.e. sound is being emitted). Safe for other
 * drivers (e.g. the battery gauge) to probe load state without touching
 * audio internals. */
bool hw_audio_is_playing(void);

/* Bytes still queued in the PCM ring (decoded but not yet pulled by the
 * writer task). The player polls this after a track hits EOF to learn when
 * every decoded sample has at least been handed to the DMA — i.e. when the
 * song has really ended, not merely when the decoder reached file-EOF. The
 * DMA adds a bounded ~280 ms tail on top; hw_audio_drain_blocking() waits
 * that out too. */
size_t hw_audio_pending(void);

/* Block until every queued sample has actually been played (ring drained and
 * the DMA's in-flight tail flushed). Call at a track boundary or before park
 * so the previous track's audio is fully emitted before the next starts or the
 * device goes silent. Safe to call from the decode task (allowed to block at a
 * seam). */
void hw_audio_drain_blocking(void);
