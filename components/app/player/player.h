/*
 * Application layer: MP3 file player.
 *
 * Decodes .mp3 files from the SD card (/sdcard) using the libhelix-mp3
 * software decoder and streams raw PCM to the I2S audio driver. Decoding runs
 * in a dedicated FreeRTOS task so the LVGL UI stays responsive.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MP3_NAME_LEN 64

/* Max length of a fully-qualified track path ("/sdcard/<name>"). Kept in one
 * place so the player / ebook buffers and the snprintf() calls below never
 * disagree and can never overflow. */
#define PLAYER_PATH_LEN 192

typedef enum {
    PLAYER_IDLE = 0,
    PLAYER_PLAYING,
    PLAYER_PAUSED,
} player_state_t;

/* Create the player task and the background scan task. Call once at startup. */
void player_init(void);

/* --- Track list (background scan) -------------------------------------
 * Walking the FATFS directory is slow (tens of ms on SDSPI), so the list is
 * built on a dedicated task and never blocks the UI: request a scan with
 * player_scan_start() and poll player_scan_version() / player_scan_busy() to
 * learn when the cached list refreshes. */
#define PLAYER_SCAN_MAX 64

/* Kick off a background scan of /sdcard for .mp3 files. Requests arriving
 * while one is running coalesce into a single follow-up scan. */
void player_scan_start(void);

/* True while a scan is running or queued. */
bool player_scan_busy(void);

/* Bumped on every completed scan; poll to detect a fresh list. */
uint32_t player_scan_version(void);

/* Number of tracks in the last completed scan. */
int player_scan_count(void);

/* Name of track `i` from the last completed scan ("" if out of range). */
const char *player_scan_name(int i);

/* Current playback state. */
player_state_t player_state(void);

/* Basename of the track currently loaded, or "" if idle. */
const char *player_current_name(void);

/* (playback progress intentionally omitted: byte-offset percentage is
 * inaccurate for VBR MP3 and is not surfaced anywhere in the UI) */

/* Start/restart playback of the named file (basename under /sdcard). */
void player_play(const char *name);

/* Pause the current track, or resume if paused. */
void player_toggle(void);

/* Stop playback and release the I2S bus. */
void player_stop(void);
