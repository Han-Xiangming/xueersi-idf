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

typedef enum {
    PLAYER_IDLE = 0,
    PLAYER_PLAYING,
    PLAYER_PAUSED,
} player_state_t;

/* Create the player task. Call once at startup. */
void player_init(void);

/* Scan /sdcard for .mp3 files. Fills up to `max` names (each MP3_NAME_LEN
 * bytes, NUL-terminated, no path) and sets *count. Returns 0 on success. */
int player_scan(char (*names)[MP3_NAME_LEN], int max, int *count);

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
