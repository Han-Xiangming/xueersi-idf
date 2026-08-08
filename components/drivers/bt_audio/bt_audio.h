/*
 * Hardware layer: Bluetooth A2DP Source audio output.
 *
 * Turns the device into a Bluetooth audio SOURCE: decoded MP3 PCM is streamed
 * to a paired Bluetooth sink (headphones / speaker); SBC encoding is done by
 * the Bluedroid stack itself. The audio pipeline (hw_audio_write_pcm) feeds
 * PCM in via bt_audio_write_pcm(); this module owns the Bluetooth stack,
 * sink discovery/connection and a small PCM ring buffer.
 *
 * Targets ESP-IDF v5.x (verified against 5.5). Every entry point is a safe
 * no-op when Bluetooth / A2DP is disabled in the build
 * (CONFIG_BT_ENABLED && CONFIG_BT_A2DP_ENABLE).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Max characters of a device name kept in the scan list (incl. NUL). */
#define BT_DEV_NAME_LEN 32

/* Max number of discovered sinks kept in the scan list. */
#define BT_MAX_DEVICES 8

/* Allocate the Bluetooth PCM ring. Cheap and does NOT touch the BT controller;
 * call once at startup (after the local audio driver is ready). The Bluetooth
 * controller / Bluedroid stack / A2DP Source role are brought up later, on
 * demand, by bt_audio_enable() when the user opens the BLUETOOTH page. */
void bt_audio_init(void);

/* Lazily bring up the Bluetooth controller, Bluedroid stack and A2DP Source
 * role. Idempotent: the first call does the real work; subsequent calls are
 * no-ops. Call when the user opens the BLUETOOTH page so the device does not
 * advertise an A2DP source at boot. */
void bt_audio_enable(void);

/* Tear the stack back down and power off the Bluetooth controller (the
 * reverse of bt_audio_enable). Idempotent: a no-op when already down. Call
 * when the user switches the Bluetooth master switch OFF so the radio is
 * truly silent instead of merely unconnected. All discovery/connection state
 * is reset, so a later bt_audio_enable() starts clean. */
void bt_audio_disable(void);

/* Enable / disable routing of decoded PCM to the Bluetooth sink. */
void bt_audio_set_enabled(bool enabled);
bool bt_audio_is_enabled(void);

/* True once a Bluetooth A2DP sink is connected. */
bool bt_audio_is_connected(void);

/* True only while decoded PCM is actually being streamed to the sink. Use
 * this (not bt_audio_is_connected) as the routing gate so a connected-but-not
 * yet-streaming link still falls back to the speaker instead of stalling it. */
bool bt_audio_is_streaming(void);

/* --- Sink discovery / selection (drives the BLUETOOTH UI page) ---------- */

/* Clear the device list and start a GAP inquiry for audio sinks. */
void bt_audio_scan_start(void);
bool bt_audio_is_scanning(void);

/* Snapshot of the discovered audio-sink list (grows while scanning). */
int bt_audio_device_count(void);

/* Monotonic counter bumped whenever the discovered-device list changes
 * (a device added, or a previously nameless device's name filled in). The
 * UI reads the list only when this advances, instead of re-formatting the
 * device names (incl. the MAC-address fallback) on every refresh tick. */
uint32_t bt_audio_device_version(void);

const char *bt_audio_device_name(int index);

/* Connect to the index-th discovered device (cancels any running scan and
 * enables BT output). Returns false on bad index / request failure. */
bool bt_audio_connect_index(int index);

/* Drop the current A2DP connection (keeps the stack up). */
void bt_audio_disconnect(void);

/* Name of the connected (or connecting) sink, "" if none. */
const char *bt_audio_peer_name(void);

/* --- Pairing progress (shown on the BLUETOOTH UI page) ------------------ */

typedef enum {
    BT_PAIR_IDLE = 0,       /* nothing in progress */
    BT_PAIR_CONNECTING,     /* esp_a2d_source_connect() issued */
    BT_PAIR_PAIRING,        /* SSP numeric comparison: passkey available */
    BT_PAIR_OK,             /* authentication finished successfully */
    BT_PAIR_FAIL,           /* authentication / connection failed */
} bt_pair_state_t;

bt_pair_state_t bt_audio_pair_state(void);

/* 6-digit SSP passkey to show while state == BT_PAIR_PAIRING. */
uint32_t bt_audio_passkey(void);

/* Feed one decoded stereo-PCM frame (L,R interleaved int16) into the BT
 * pipeline. Only enqueues while enabled, connected and actively streaming;
 * safe (no-op) to call otherwise. */
void bt_audio_write_pcm(const int16_t *stereo_frames, size_t frames);

/* Tell the BT pipeline the sample rate of the PCM fed above. The A2DP/SBC
 * stream always runs at 44.1 kHz, so any other input rate is resampled
 * internally (otherwise the sink would play fast/slow and pitch-shifted). */
void bt_audio_set_sample_rate(uint32_t rate_hz);

/* === AVRCP remote control ===================================================
 * The device is an A2DP SOURCE, so it registers the AVRCP TARGET (TG) role and
 * lets a paired remote (the headset/speaker's media keys) drive local playback
 * and volume. Bluedroid auto-ACKs the passthrough / absolute-volume commands,
 * so this module only translates them into the application callbacks below. */

/* Media command delivered by a remote AVRCP controller. */
typedef enum {
    BT_AVRC_CMD_PLAY,   /* start / resume playback */
    BT_AVRC_CMD_PAUSE,  /* pause playback */
    BT_AVRC_CMD_STOP,   /* stop playback */
    BT_AVRC_CMD_NEXT,   /* next track */
    BT_AVRC_CMD_PREV,   /* previous track */
} bt_avrc_cmd_t;

/* Invoked when a remote media key is pressed (PLAY/PAUSE/STOP/NEXT/PREV). */
typedef void (*bt_avrc_cmd_cb_t)(bt_avrc_cmd_t cmd);

/* Invoked with the absolute volume sent by the remote, 0..127 (AVRCP full
 * scale). Map it to a 0..100% master volume on the application side. */
typedef void (*bt_avrc_volume_cb_t)(uint8_t volume_0_127);

/* Register handlers for remote AVRCP commands. Safe to call at any time;
 * passing NULL disables the corresponding handler. */
void bt_audio_set_avrc_cmd_cb(bt_avrc_cmd_cb_t cb);
void bt_audio_set_avrc_volume_cb(bt_avrc_volume_cb_t cb);
