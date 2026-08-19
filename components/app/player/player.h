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

/* Max length of a track file NAME (basename under /sdcard). Sized to the FATFS
 * long-file-name limit (255 bytes) plus one NUL, so multi-byte (e.g. Japanese)
 * titles are never truncated here — truncation for DISPLAY is the UI's job, not
 * the player's, and a truncated name would fail to open on the SD card. */
/* NOTE: MP3_NAME_LEN is 256 bytes. Do NOT declare a stack-local buffer of this
 * size (e.g. `char tmp[MP3_NAME_LEN];`) inside a UI/refresh function — the lvgl
 * task stack is only 10 KB and such an array overflows it under load, corrupting
 * adjacent memory (seen as a SPI-bus ISR Guru Meditation). Use a `static` buffer
 * or the dedicated display-width clipped copy instead. */
#define MP3_NAME_LEN 256

/* SD card mount point. The playlist is built by scanning the WHOLE card
 * (every directory) for .mp3 files, so there is no single fixed music folder. */
#define PLAYER_ROOT "/sdcard"

/* On-card playlist cache: a flat text file so we avoid pulling in a JSON
 * parser. One record per line, two TAB-separated fields:
 *     <title> <TAB> <path> <LF>
 * `title` is the display basename (redundant: it is always the basename of
 * `path`; kept so older cache files written before the entry was slimmed
 * down still load); `path` is the absolute path. The source is implicitly
 * PL_SRC_FOLDER (the only one implemented today). A line that cannot be
 * parsed invalidates the whole cache, so a corrupt file never freezes the
 * UI into a broken state — we just fall back to a real scan. */
#define PLAYER_CACHE_FILE "/sdcard/.xueersi_playlist.cache"

/* Max length of a fully-qualified track path anywhere on the SD card.
 * A FATFS path is at most 80 bytes by spec, but long-file-name leaf names can
 * reach 255 bytes; size to the worst case (mount point + deepest path) with
 * headroom. Kept in one place so every buffer/snprintf agrees and can never
 * overflow. */
#define PLAYER_PATH_LEN 320

typedef enum {
    PLAYER_IDLE = 0,
    PLAYER_PLAYING,
    PLAYER_PAUSED,
} player_state_t;

/* Create the player task, the background scan task and the stall watchdog
 * task. Call once at startup. */
void player_init(void);

/* --- Playback errors ---------------------------------------------------
 * The player reports WHY a track/playback failed instead of failing silent.
 * The error is sticky: it is set when a track is aborted (corrupt file,
 * pipeline stall, ...) and cleared automatically once a NEW track decodes
 * its first frame successfully. The UI surfaces it as a toast. */
typedef enum {
    PLAYER_ERR_NONE = 0,   /* all good */
    PLAYER_ERR_OPEN,       /* track file open / decoder init failed */
    PLAYER_ERR_CORRUPT,    /* aborted: no MP3 sync word / too many decode errors */
    PLAYER_ERR_PIPELINE,   /* audio pipeline stalled repeatedly (I2S DMA wedged) */
    PLAYER_ERR_STALL,      /* decode made no progress for a long time (SD/BT hang) */
    PLAYER_ERR_AUDIO,      /* play requested while the audio pipeline is not ready */
} player_err_t;

/* Last playback error (PLAYER_ERR_NONE when everything is fine). */
player_err_t player_last_error(void);

/* Short Chinese description of an error code, for the UI toast. */
const char *player_err_text(player_err_t err);

/* --- Playlist (background load) ---------------------------------------
 * The playlist is an IMMUTABLE ordered snapshot. Walking the FATFS tree is
 * slow (tens of ms per directory on SDSPI), so the list is built on a
 * dedicated task and never blocks the UI: request a (re)load with
 * player_scan_start() and poll player_scan_version() / player_scan_busy() to
 * learn when the cached list refreshes.
 *
 * Source abstraction: a playlist entry is a fully-qualified absolute path;
 * the display name is derived as the path's basename (no duplicate storage,
 * so the entry is only PLAYER_PATH_LEN bytes). The playback layer is
 * source-agnostic — it only ever sees the published playlist_t. Today only
 * PL_SRC_FOLDER is implemented (recursive scan of the whole SD card);
 * PL_SRC_M3U is reserved so a future playlist-file source fills the SAME
 * entry array without touching playback.
 *
 * ORDER IS LOCKED: no runtime code may reorder or mutate a published
 * snapshot. The only way to change the order is to reload a fresh snapshot
 * (which, for the folder source, means the filesystem order + a stable sort).
 * There is deliberately NO move/shuffle/reorder API. */
#define PLAYER_SCAN_MAX 256

/* Playlist source. Only PL_SRC_FOLDER is implemented now; PL_SRC_M3U is a
 * placeholder for a future playlist-file source (no playback-layer change). */
typedef enum {
    PL_SRC_FOLDER = 0,   /* recursive scan of the whole SD card */
    /* PL_SRC_M3U, */    /* reserved: parse a .m3u/.m3u8 file */
} playlist_src_t;

/* A single playlist entry. `path` is an absolute path so folder- and
 * file-sourced playlists are structurally identical at playback time; the
 * display name is `strrchr(path, '/') + 1` (basename), never stored twice. */
typedef struct {
    char path[PLAYER_PATH_LEN];   /* absolute path, e.g. /sdcard/Album/a.mp3 */
} playlist_entry_t;

/* Published, read-only playlist snapshot. Double-buffered (see player.c): a
 * loader fills one buffer, then atomically swaps the pointer, so readers
 * always see a complete, consistent list. Once published, NEVER written. */
typedef struct {
    playlist_entry_t items[PLAYER_SCAN_MAX];
    int count;
    uint32_t version;             /* bumped on every (re)load; UI polls this */
    playlist_src_t src;           /* which source produced this snapshot */
} playlist_t;

/* Kick off a background (re)load of the playlist from the given source.
 * `root` is the directory to scan for the folder source (e.g. "/sdcard" for
 * the whole card, or "/sdcard/Album" for one folder). Ignored for other
 * sources. Requests arriving while one is running coalesce into a single
 * follow-up load. Only PL_SRC_FOLDER is meaningful today. */
void player_load(playlist_src_t src, const char *root);

/* Backward-compatible alias: reload the whole-card folder source. */
static inline void player_scan_start(void)
{
    player_load(PL_SRC_FOLDER, PLAYER_ROOT);
}

/* Load the whole-card playlist, preferring the on-card cache for an
 * instantaneous start. If the cache is missing or unreadable we fall back to
 * a background scan and (re)write the cache on completion. Call this from
 * player_init() in place of player_scan_start() so a re-entered player never
 * blocks on the FATFS walk. Repeated calls are cheap: the cache is
 * validated by file fingerprint (size+mtime) and skipped entirely when the
 * published snapshot is already the unchanged whole-card list. */
void player_scan_with_cache(void);

/* Force a fresh scan and rewrite the cache (e.g. the user asked to rebuild
 * the list from the source picker). Drops the cache file first so a failed
 * scan never leaves a half-stale cache behind. */
void player_rescan(void);

/* True if a readable playlist cache exists on the SD card right now. The UI
 * uses this to label the refresh affordance ("有缓存" vs "无缓存"). */
bool player_cache_exists(void);

/* Human-readable name of the source currently loaded, for the UI (e.g.
 * "整卡" for the whole card, or the folder's basename). "" if none. */
const char *player_current_src_name(void);

/* True while a load is running or queued. */
bool player_scan_busy(void);

/* Bumped on every completed load; poll to detect a fresh list. */
uint32_t player_scan_version(void);

/* Number of tracks in the last completed load. */
int player_scan_count(void);

/* Name of track `i` from the last completed load ("" if out of range). */
const char *player_scan_name(int i);

/* Absolute path of track `i` ("" if out of range). Use this for playback. */
const char *player_scan_path(int i);

/* Current playback state. */
player_state_t player_state(void);

/* Basename of the track currently loaded, or "" if idle. */
const char *player_current_name(void);

/* (playback progress intentionally omitted: byte-offset percentage is
 * inaccurate for VBR MP3 and is not surfaced anywhere in the UI) */

/* Start/restart playback. `path` is treated as an absolute path if it starts
 * with '/', otherwise it is resolved under PLAYER_ROOT (legacy basename use).
 * Passing player_scan_path(i) is the normal, source-agnostic way to play. */
void player_play(const char *path);

/* Play the track at list index `i` (0-based). Out-of-range indices are
 * ignored. */
void player_play_index(int i);

/* Index of the track currently loaded (0-based), or -1 when nothing has been
 * loaded yet / the list is empty. */
int player_current_index(void);

/* Move to the next/previous track relative to the current one, wrapping at
 * the list ends. While a track is loaded (playing or paused) the switch is
 * immediate; when idle they just report the index a next/prev would land on
 * (the UI moves its cursor without starting playback). Returns the index of
 * the target track, or -1 when the list is empty. */
int player_next(void);
int player_prev(void);

/* Pause the current track, or resume if paused. */
void player_toggle(void);

/* Stop playback and release the I2S bus. */
void player_stop(void);

/* --- Repeat mode -------------------------------------------------------
 * How the player behaves when a track reaches its natural end. Default is
 * list loop (advance to the next entry, wrapping at the end); single-track
 * loop replays the current track; random plays a random entry (never the
 * one that just ended, unless the list has a single track). In random mode
 * a manual next also picks randomly; prev stays sequential so the user can
 * always step back. Changed at runtime by the UI (Select key on the player
 * page, 循环 control on the floating playback panel). Not persisted to NVS:
 * each boot starts in list-loop mode. */
typedef enum {
    PLAYER_REPEAT_ALL = 0,   /* list loop: next track at end (default) */
    PLAYER_REPEAT_ONE,       /* single-track loop: replay the current track */
    PLAYER_REPEAT_RANDOM,    /* random: play a random track at end / on next */
} player_repeat_t;

/* Current repeat mode. */
player_repeat_t player_repeat_mode(void);

/* Cycle the repeat mode: list loop -> single-track -> random -> list loop. */
void player_repeat_toggle(void);
