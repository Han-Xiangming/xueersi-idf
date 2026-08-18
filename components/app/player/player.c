/*
 * Application layer: MP3 file player.
 * See app/player.h.
 */
#define LOG_LOCAL_LEVEL ESP_LOG_INFO    /* keep per-50-frame decode tracing out unless explicitly set to DEBUG at runtime */
#include "player.h"
#include "audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#include "mp3dec.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "rtc_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if defined(CONFIG_ESP_TASK_WDT_EN)
#include "esp_task_wdt.h"
#endif

/* Read buffer for MP3 stream data. */
#define MP3_READ_CHUNK  (4 * 1024)
/* Worst-case decoded PCM: MPEG1 Layer III, 2 channels, 1152 samples/frame. */
#define MP3_PCM_MAX     (2 * 1152 * 2)

/* Consecutive decode failures that mark a track corrupt. Resyncing garbage
 * one byte at a time is O(n^2) on a bad region; past this cap the track is
 * aborted and the player advances to the next one (or stops) instead of
 * spinning the CPU forever on a file that will never produce audio. */
#define TRACK_MAX_DECODE_ERRS    512
/* Consecutive AUDIO_WRITE_STALLED results that abort a track. Each stall is
 * a bounded I2S write timing out, which proves the DMA is not consuming; a
 * few in a row mean the pipeline is wedged, not just momentarily slow. */
#define TRACK_MAX_PIPELINE_STALLS 3
/* Consecutive tracks that failed to play before the player gives up and
 * stops (avoids cycling through a whole card of corrupt files forever). */
#define TRACK_MAX_CONSEC_FAILS   8
/* Decode-progress watchdog: while PLAYING, if no frame has been produced for
 * this long, the decode task is stuck (SD read hang, BT send hang, ...) and
 * the watchdog stops playback instead of faking an endless "playing" state. */
#define PLAYER_STALL_MS          12000

static const char *TAG = "player";

static TaskHandle_t s_task = NULL;
static volatile player_state_t s_state = PLAYER_IDLE;
/* Repeat mode at natural end of a track. Cross-task (UI toggles it, the
 * decode task reads it), so volatile like the other control flags. */
static volatile player_repeat_t s_repeat = PLAYER_REPEAT_ALL;

static char s_path[PLAYER_PATH_LEN];        /* full path: /sdcard/Music/<name> */
static char s_name[MP3_NAME_LEN];
static int s_index = -1;          /* list index of the loaded track (-1 = none) */
static volatile bool s_stop_req;
static volatile bool s_pause_req;
static volatile bool s_new_req;
static char s_new_path[PLAYER_PATH_LEN];
static char s_new_name[MP3_NAME_LEN];
static uint32_t s_dbg_frames;          /* decode-frame counter for debug logs */
static uint32_t s_dbg_rate;            /* samplerate captured from 1st frame */
/* Consecutive buffer refills that failed to locate an MP3 sync word. Reset on
 * each new track; if it reaches the limit the track is aborted instead of
 * spinning forever on a corrupt/non-MP3 file. */
static int s_no_sync_refills;
/* Consecutive MP3Decode failures in the current track (reset on success).
 * Guards against the byte-by-byte resync spinning forever on garbage. */
static int s_track_errs;
/* Consecutive AUDIO_WRITE_STALLED results in the current track. */
static int s_pcm_stalls;
/* Consecutive tracks that failed to play (open/decode/pipeline). */
static int s_fail_count;
/* Set when the current track is being aborted because of an error (as
 * opposed to a natural EOF), so the end-of-track logic can auto-advance
 * instead of replaying a broken track under REPEAT_ONE. */
static bool s_track_errored;
/* Last playback error, sticky until the next track decodes its first frame
 * successfully. Read by the UI task, written by the player/watchdog tasks;
 * single-word stores are atomic on Xtensa. */
static volatile player_err_t s_last_err = PLAYER_ERR_NONE;
/* Monotonic timestamp (ms, esp_timer) of the last decode progress. Read by
 * the watchdog task while the player is PLAYING to detect a hung decode. */
static volatile uint32_t s_decode_beat_ms;
/* True while decode_loop() is running (including paused inside it). Lets
 * player_play() distinguish a self-call (single-track replay / list
 * auto-advance) from a UI call: a self-call must NOT set s_stop_req — the
 * track has already ended, and setting it would let a user stop pressed in
 * the tiny switch window be eaten by the loop top. (Single-task read/write:
 * only the player task and the UI task touch it, and only via player_play.) */
static volatile bool s_in_decode_loop;

/* Abstract data source: a file on the SD card. */
typedef struct {
    FILE *fp;
} track_src_t;

/* Decoder working buffers (owned by the decode loop). s_pcm stays in
 * internal DRAM on purpose: for stereo files (the common case) it is the
 * buffer handed to hw_audio_write_pcm (DSP in place, then written to the
 * I2S DMA) at ~40 Hz, and a PSRAM source there puts the cache-workaround
 * copy on the hot path (documented crash source under BT controller load).
 * s_stereo, used only for mono files, keeps the PSRAM placement to fit the
 * DRAM budget. */
static track_src_t s_src;
static HMP3Decoder s_dec;
static unsigned char s_readbuf[MP3_READ_CHUNK];
static int s_bytes_left;
static int s_consumed;
static int16_t s_pcm[MP3_PCM_MAX];
EXT_RAM_BSS_ATTR static int16_t s_stereo[MP3_PCM_MAX];

/* --- Playback error reporting ------------------------------------------
 * Errors are sticky: set when a track is aborted / a play request is
 * rejected, cleared once a new track decodes its first frame successfully
 * (real audio progress), so the UI can show why playback stopped. */
const char *player_err_text(player_err_t err)
{
    switch (err) {
    case PLAYER_ERR_OPEN:     return "打开失败";
    case PLAYER_ERR_CORRUPT:  return "文件损坏";
    case PLAYER_ERR_PIPELINE: return "音频卡住";
    case PLAYER_ERR_STALL:    return "播放无响应";
    case PLAYER_ERR_AUDIO:    return "音频不可用";
    case PLAYER_ERR_NONE:
    default:                  return "";
    }
}

static void player_report_error(player_err_t err)
{
    if (err != s_last_err) {
        s_last_err = err;
        ESP_LOGE(TAG, "player error -> %s", player_err_text(err));
    }
}

player_err_t player_last_error(void)
{
    return s_last_err;
}

/* --- Background playlist load ------------------------------------------
 * opendir/readdir over SDSPI is slow (tens of ms), so the track list is built
 * on its own task. The result is published as an IMMUTABLE snapshot via
 * double buffering: a loader fills one playlist_t buffer, then atomically
 * swaps the s_playlist pointer, so readers (UI / decode loop) always see a
 * complete, consistent list and never a half-written one. The UI polls
 * s_playlist->version at its 16 ms cadence, so it never blocks on the load.
 * EXT_RAM_BSS keeps the two playlist buffers out of internal DRAM. */

/* Double buffer: loaders write one, readers see the other via s_playlist. */
EXT_RAM_BSS_ATTR static playlist_t s_pl_a;
EXT_RAM_BSS_ATTR static playlist_t s_pl_b;
/* The live, published snapshot. Swapped atomically (32-bit Xtensa word
 * store is atomic) after a buffer is fully filled. READ-ONLY after publish. */
static playlist_t *volatile s_playlist = &s_pl_a;

static int s_scan_count;          /* mirrored from s_playlist->count for the compat shell */
static uint32_t s_scan_version;   /* mirrored from s_playlist->version */
static bool s_scan_busy;
static bool s_scan_pending;
static TaskHandle_t s_scan_task;
static TaskHandle_t s_watch_task;   /* decode-stall watchdog (see player_watch_task) */

/* The whole-card cache is validated like an HTTP ETag / Last-Modified
 * before any re-read: we remember the cache file's size + mtime at load and
 * write time, then stat() again before touching it — so repeated player
 * entries and <ALL> selections skip the SD I/O entirely when nothing
 * changed. The fingerprint turns stale after: the file changed, the SD was
 * removed (stat fails), or an explicit rescan dropped the cache. */
static bool s_pub_is_whole_card;   /* published snapshot == whole-card list */
static bool s_cache_fp_valid;      /* s_cache_fp holds a valid fingerprint */
static struct stat s_cache_fp;

static void playlist_cache_refresh_fp(void)
{
    s_cache_fp_valid = (stat(PLAYER_CACHE_FILE, &s_cache_fp) == 0);
}

/* True when the cache file still looks byte-identical to what we last loaded
 * or wrote (same size and mtime). False if it is gone (SD removed / rescan
 * unlinked it) or no reference fingerprint has been captured yet. */
static bool playlist_cache_unchanged(void)
{
    struct stat st;
    if (!s_cache_fp_valid || stat(PLAYER_CACHE_FILE, &st) != 0) {
        return false;
    }
    return st.st_size == s_cache_fp.st_size && st.st_mtime == s_cache_fp.st_mtime;
}

/* Forward declaration: a completed real scan persists its result to the
 * on-card cache so the next player entry is instantaneous. Defined further
 * down with the cache read/parse helpers. */
static void playlist_cache_write(const playlist_t *pl);
static int playlist_load_from_cache(playlist_t *work);

/* Compare two entries by their basename, case-insensitive, for a stable
 * folder order (mirrors the display name used elsewhere). */
static int playlist_entry_cmp(const void *a, const void *b)
{
    const playlist_entry_t *pa = (const playlist_entry_t *)a;
    const playlist_entry_t *pb = (const playlist_entry_t *)b;
    const char *ba = strrchr(pa->path, '/');
    const char *bb = strrchr(pb->path, '/');
    ba = (ba != NULL) ? ba + 1 : pa->path;
    bb = (bb != NULL) ? bb + 1 : pb->path;
    return strcasecmp(ba, bb);
}

/* Publish a filled work buffer as the new live snapshot (atomic pointer
 * swap). `work` must be one of s_pl_a / s_pl_b NOT currently live. */
static void playlist_publish(playlist_t *work, int n, playlist_src_t src)
{
    work->count = n;
    work->src = src;
    work->version = s_scan_version + 1;   /* monotonic; UI sees a fresh list */
    s_playlist = work;                    /* atomic swap: readers now see it */
    s_scan_count = n;
    s_scan_version = work->version;
}

/* Recursively collect .mp3 files under `dir` into `work`, in-place. `depth`
 * bounds recursion so a pathological symlink/cycle can't overflow the stack.
 * Returns the running total; stops early once PLAYER_SCAN_MAX is reached. */
static int playlist_collect_dir(playlist_t *work, int n, const char *dir,
                                int depth)
{
    if (depth > 16 || n >= PLAYER_SCAN_MAX) {
        return n;
    }
    DIR *d = opendir(dir);
    if (d == NULL) {
        return n;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < PLAYER_SCAN_MAX) {
        const char *fn = e->d_name;
        if (fn[0] == '.') {                 /* skip ".", "..", hidden */
            continue;
        }
        char child[PLAYER_PATH_LEN];
        snprintf(child, sizeof(child), "%s/%s", dir, fn);

        /* Decide file vs directory without relying on d_type (unreliable on
         * FATFS): stat the entry. A directory is recursed into; a regular
         * file ending in .mp3 is added. */
        struct stat st;
        if (stat(child, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            n = playlist_collect_dir(work, n, child, depth + 1);
        } else if (S_ISREG(st.st_mode)) {
            int len = (int)strlen(fn);
            if (len > 4 && strcasecmp(fn + len - 4, ".mp3") == 0) {
                snprintf(work->items[n].path, sizeof(work->items[n].path),
                         "%s", child);
                n++;
            }
        }
    }
    closedir(d);
    return n;
}

/* Load the playlist from a folder source: a recursive scan of the WHOLE SD
 * card. Fills the idle buffer and publishes it. A future playlist-file source
 * would fill the SAME entry array (keeping file order) and call
 * playlist_publish() — the playback layer is unchanged. */
/* Name of the source currently loaded, for the UI. "整卡" when the root is
 * the whole card, else the folder's basename. */
static char s_src_name[MP3_NAME_LEN];

static void player_load_folder(const char *root)
{
    playlist_t *work = (s_playlist == &s_pl_a) ? &s_pl_b : &s_pl_a;
    int n = playlist_collect_dir(work, 0, root, 0);
    /* readdir/stat order is arbitrary across directories; sort by name so the
     * list is stable across reloads. Folder source only — a playlist-file
     * source keeps its file order and would skip this sort. */
    if (n > 1) {
        qsort(work->items, (size_t)n, sizeof(playlist_entry_t),
              playlist_entry_cmp);
    }
    /* Remember the display name: the whole-Music source -> "整卡", else the
     * selected sub-folder's basename. Bounded copy so GCC's
     * -Wformat-truncation stays quiet (root can be up to PLAYER_PATH_LEN
     * bytes; FATFS leaf names cap at 255). */
    const char *b = strrchr(root, '/');
    const char *base = (b != NULL) ? b + 1 : root;
    if (strcasecmp(base, "Music") == 0) {
        snprintf(s_src_name, sizeof(s_src_name), "整卡");
    } else {
        size_t blen = strnlen(base, MP3_NAME_LEN - 1);
        memcpy(s_src_name, base, blen);
        s_src_name[blen] = '\0';
    }
    playlist_publish(work, n, PL_SRC_FOLDER);
    /* Remember what we just published: the whole-card list only when the
     * scan root was the card root (drives the cache fingerprint reuse). */
    s_pub_is_whole_card = (strcmp(root, PLAYER_ROOT) == 0);
    /* The on-card cache represents the WHOLE-CARD list only. A sub-folder
     * browse must NOT overwrite it, or the next player entry (which always
     * loads the cache as "整卡") would show the wrong list. Persist the
     * cache solely when scanning the whole card. */
    if (strcmp(root, PLAYER_ROOT) == 0) {
        playlist_cache_write(work);
    }
}

const char *player_current_src_name(void)
{
    return s_src_name;
}

/* The directory the next/last folder load scans. Defaults to the whole card
 * (PLAYER_ROOT). Set by player_load(); read by scan_task. Declared at file
 * scope (before scan_task) so the loader task can see it. */
static char s_load_root[PLAYER_PATH_LEN] = PLAYER_ROOT;

static void scan_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        s_scan_busy = true;
        do {
            s_scan_pending = false;
            player_load_folder(s_load_root);
        } while (s_scan_pending);   /* a request landed mid-load: redo */
        s_scan_busy = false;
    }
}

void player_load(playlist_src_t src, const char *root)
{
    /* Only the folder source is implemented; ignore unknown sources. A future
     * PL_SRC_M3U would dispatch to a player_load_m3u() here without any change
     * to the playback/loop logic. */
    if (src != PL_SRC_FOLDER) {
        ESP_LOGW(TAG, "playlist source %d not implemented", src);
        return;
    }
    if (root == NULL) {
        root = PLAYER_ROOT;
    }
    /* Whole-card requests are served from the on-card cache (or the
     * in-memory snapshot) instead of a full FATFS walk: the cache IS the
     * whole-card list, and staleness is handled by the explicit rescan.
     * Only a sub-folder request needs a real scan. */
    if (strcmp(root, PLAYER_ROOT) == 0 && !s_scan_busy && !s_scan_pending) {
        if (s_pub_is_whole_card && playlist_cache_unchanged()) {
            return;   /* already showing the fresh whole-card list */
        }
        playlist_t *work = (s_playlist == &s_pl_a) ? &s_pl_b : &s_pl_a;
        if (playlist_load_from_cache(work) > 0) {
            return;   /* served from cache; no walk needed */
        }
        /* Cache unusable: fall through to a real scan (which rewrites it). */
    }
    strncpy(s_load_root, root, sizeof(s_load_root) - 1);
    s_load_root[sizeof(s_load_root) - 1] = '\0';
    s_scan_pending = true;
    if (s_scan_task != NULL) {
        xTaskNotifyGive(s_scan_task);
    }
}

bool player_scan_busy(void)
{
    return s_scan_busy || s_scan_pending;
}

uint32_t player_scan_version(void)
{
    return s_scan_version;
}

int player_scan_count(void)
{
    return s_scan_count;
}

const char *player_scan_name(int i)
{
    if (i < 0 || i >= s_scan_count) {
        return "";
    }
    const char *b = strrchr(s_playlist->items[i].path, '/');
    return (b != NULL) ? b + 1 : s_playlist->items[i].path;
}

const char *player_scan_path(int i)
{
    if (i < 0 || i >= s_scan_count) {
        return "";
    }
    return s_playlist->items[i].path;
}

/* --- ID3v2 skipping + ReplayGain ---------------------------------------
 * The MP3 decoder must never see the ID3v2 tag at the head of the file (its
 * bytes can lock MP3FindSyncWord onto a bogus 0xFFEx frame header and wreck
 * the sample rate), so the tag is skipped. While we are there the tag's
 * frames are scanned for ReplayGain 2.0 metadata (written by PC tools like
 * loudgain): TXXX frames "REPLAYGAIN_TRACK_GAIN" / "REPLAYGAIN_ALBUM_GAIN"
 * (ID3v2.3 / v2.4) or RVA2 frames (ID3v2.4). The gain is pushed into the
 * audio DSP chain as a per-track gain so quiet and loud sources come out at
 * a comparable level (see hw_audio_set_track_gain_db). Parsing is
 * best-effort: any malformed tag simply yields 0 dB. An ID3v2 tag starts
 * with the magic "ID3" + 7 header bytes; the 4 size bytes are "syncsafe"
 * (top bit always 0), so the tag length is (b0<<21)|(b1<<14)|(b2<<7)|b3
 * + 10 header bytes. */

static long id3_big32(const unsigned char *b)
{
    return ((long)b[0] << 24) | ((long)b[1] << 16) |
           ((long)b[2] << 8)  | (long)b[3];
}

static long id3_syncsafe32(const unsigned char *b)
{
    return ((long)(b[0] & 0x7f) << 21) | ((long)(b[1] & 0x7f) << 14) |
           ((long)(b[2] & 0x7f) << 7)  | (long)(b[3] & 0x7f);
}

/* Match `ascii` against a TXXX description that may be ISO-8859-1 / UTF-8
 * (1 byte/char) or UTF-16 (chars interleaved with NUL bytes). Skipping NUL
 * bytes makes the comparison encoding-agnostic. */
static bool id3_desc_matches(const unsigned char *d, size_t n,
                             const char *ascii)
{
    size_t k = 0;
    for (size_t i = 0; i < n && ascii[k] != '\0'; i++) {
        if (d[i] == 0x00) {
            continue;   /* UTF-16 interleave */
        }
        if (d[i] != (unsigned char)ascii[k]) {
            return false;
        }
        k++;
    }
    return ascii[k] == '\0';
}

/* Parse a dB value from the tail of a text frame, skipping UTF-16 NUL
 * interleave and BOM bytes, stopping at the first non-number character
 * ("-6.23 dB" -> -6.23). */
static bool id3_parse_db(const unsigned char *d, size_t n, float *out)
{
    char buf[32];
    size_t k = 0;
    bool started = false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = d[i];
        if (c == 0x00 || c == 0xFF || c == 0xFE) {
            continue;   /* UTF-16 interleave / BOM bytes */
        }
        if (!started) {
            if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.') {
                started = true;
                buf[k++] = (char)c;
            }
            continue;   /* skip junk before the number */
        }
        if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E') {
            buf[k++] = (char)c;
        }
        else {
            break;      /* e.g. the " dB" suffix */
        }
        if (k + 1 >= sizeof(buf)) {
            break;
        }
    }
    if (!started) {
        return false;
    }
    buf[k] = '\0';
    *out = strtof(buf, NULL);
    return true;
}

/* Skip the ID3v2 tag (if any) at the head of the open file past the
 * decoder, and extract ReplayGain metadata into the audio DSP. */
static void parse_id3v2(void)
{
    float track_gain = 0.0f;
    float album_gain = 0.0f;
    bool has_track = false;
    bool has_album = false;
    unsigned char hdr[10];
    long base = ftell(s_src.fp);
    if (fread(hdr, 1, sizeof(hdr), s_src.fp) != sizeof(hdr) ||
        hdr[0] != 'I' || hdr[1] != 'D' || hdr[2] != '3') {
        fseek(s_src.fp, base, SEEK_SET);   /* not an ID3v2 tag: rewind */
        hw_audio_set_track_gain_db(0.0f);
        return;
    }
    long tag_size = id3_syncsafe32(hdr + 6);
    if (hdr[3] < 3) {
        /* ID3v2.2 and older: 3-byte frame IDs, never carries ReplayGain. */
        fseek(s_src.fp, base + 10 + tag_size, SEEK_SET);
        hw_audio_set_track_gain_db(0.0f);
        return;
    }
    /* A corrupt size field can claim more bytes than the file holds.
     * Seeking past EOF would make every later read return 0 (the track
     * silently plays nothing), and the frame walk below would grind across
     * the whole bogus span on SDSPI (minutes, no WDT feed -> reboot).
     * Trust the file: when the declared tag overruns it, seek straight
     * after the 10-byte header and let the decoder resync onto the real
     * audio frames (MP3FindSyncWord scans byte-wise). */
    long flen;
    if (fseek(s_src.fp, 0, SEEK_END) == 0 && (flen = ftell(s_src.fp)) >= 0) {
        if (base + 10 + tag_size > flen) {
            ESP_LOGW(TAG, "ID3v2 size %ld overruns file (%ld bytes), resync",
                     tag_size, flen);
            fseek(s_src.fp, base + 10, SEEK_SET);
            hw_audio_set_track_gain_db(0.0f);
            return;
        }
    }
    long tag_end = base + 10 + tag_size;
    long pos = base + 10;
    if (hdr[5] & 0x40) {
        /* Extended header (flag 0x40). Its size field excludes itself in
         * v2.4 (syncsafe) but includes itself in v2.3 (plain big-endian). */
        unsigned char eh[4];
        if (fseek(s_src.fp, pos, SEEK_SET) == 0 &&
            fread(eh, 1, sizeof(eh), s_src.fp) == sizeof(eh)) {
            pos += (hdr[3] >= 4) ? id3_syncsafe32(eh) : id3_big32(eh);
        }
        else {
            pos = tag_end;   /* header unreadable: no frames to scan */
        }
    }

    /* Bounded frame walk: even with the file-length check above, a
     * pathological tag can still claim a large-but-valid size full of
     * non-padding garbage (no 0x00 frame IDs). Cap the iterations so the
     * worst case is a few hundred SD seeks (~0.3 s), far under the task
     * WDT / stall-watchdog limits. */
    int frames_scanned = 0;
    while (pos + 10 <= tag_end && frames_scanned++ < 512) {
        unsigned char fh[10];
        if (fseek(s_src.fp, pos, SEEK_SET) != 0 ||
            fread(fh, 1, sizeof(fh), s_src.fp) != sizeof(fh)) {
            break;
        }
        long fsz = (hdr[3] >= 4) ? id3_syncsafe32(fh + 4) : id3_big32(fh + 4);
        if (fh[0] == 0x00 || fsz <= 0) {
            break;   /* padding (or corrupt): done */
        }
        if (memcmp(fh, "TXXX", 4) == 0) {
            unsigned char d[96];
            size_t rd = (fsz < (long)sizeof(d)) ? (size_t)fsz : sizeof(d);
            if (fread(d, 1, rd, s_src.fp) == rd && rd >= 3) {
                int enc = d[0];
                size_t dstart = 1;   /* first description byte */
                if (enc == 1 || enc == 2) {
                    /* UTF-16 with BOM (0xFF 0xFE = LE, 0xFE 0xFF = BE):
                     * the BOM sits between the encoding byte and the text,
                     * so the description really starts after it. */
                    if (rd >= 3 && ((d[1] == 0xFF && d[2] == 0xFE) ||
                                    (d[1] == 0xFE && d[2] == 0xFF))) {
                        dstart = 3;
                    }
                }
                size_t de = dstart;   /* description terminator */
                if (enc == 1 || enc == 2) {
                    while (de + 1 < rd &&
                           !(d[de] == 0x00 && d[de + 1] == 0x00)) {
                        de++;
                    }
                }
                else {
                    while (de < rd && d[de] != 0x00) {
                        de++;
                    }
                }
                size_t vs = de + ((enc == 1 || enc == 2) ? 2 : 1);
                if (de > dstart && vs < rd) {
                    if (id3_desc_matches(d + dstart, de - dstart,
                                         "REPLAYGAIN_TRACK_GAIN")) {
                        has_track = id3_parse_db(d + vs, rd - vs, &track_gain);
                    }
                    else if (id3_desc_matches(d + dstart, de - dstart,
                                              "REPLAYGAIN_ALBUM_GAIN")) {
                        has_album = id3_parse_db(d + vs, rd - vs, &album_gain);
                    }
                }
            }
        }
        else if (memcmp(fh, "RVA2", 4) == 0) {
            /* identification (NUL-terminated Latin-1) + channel type byte
             * (1=right, 2=left, 3=both) + 16-bit signed gain in 0.01 dB
             * steps + peak field (ignored). */
            unsigned char d[128];
            size_t rd = (fsz < (long)sizeof(d)) ? (size_t)fsz : sizeof(d);
            if (fread(d, 1, rd, s_src.fp) == rd && rd >= 6) {
                size_t idlen = 0;
                while (idlen < rd && d[idlen] != 0x00) {
                    idlen++;
                }
                bool is_track = (idlen == 5 && memcmp(d, "track", 5) == 0);
                bool is_album  = (idlen == 5 && memcmp(d, "album", 5) == 0);
                if ((is_track || is_album) && idlen + 4 <= rd) {
                    int chan = d[idlen + 1];
                    int g = (int16_t)(((int)d[idlen + 2] << 8) | d[idlen + 3]);
                    if (chan == 1 || chan == 2 || chan == 3) {
                        if (is_track) {
                            has_track = true;
                            track_gain = (float)g / 100.0f;
                        }
                        else {
                            has_album = true;
                            album_gain = (float)g / 100.0f;
                        }
                    }
                }
            }
        }
        pos += 10 + fsz;   /* loop top fseeks anyway; this bounds the walk */
    }

    fseek(s_src.fp, base + 10 + tag_size, SEEK_SET);
    ESP_LOGD(TAG, "ID3v2 tag (%ld bytes): track %.2f dB, album %.2f dB",
             tag_size, track_gain, album_gain);
    /* ReplayGain 2.0 rule: track gain wins, album gain is the fallback. */
    float gain = has_track ? track_gain : (has_album ? album_gain : 0.0f);
    hw_audio_set_track_gain_db(gain);
}

static bool open_track(void)
{
    s_src.fp = fopen(s_path, "rb");
    if (s_src.fp == NULL) {
        ESP_LOGE(TAG, "open %s failed", s_path);
        return false;
    }

    s_dec = MP3InitDecoder();
    if (s_dec == NULL) {
        ESP_LOGE(TAG, "MP3InitDecoder failed");
        fclose(s_src.fp);
        s_src.fp = NULL;
        return false;
    }
    parse_id3v2();   /* skip the tag past the decoder + pick up ReplayGain */
    s_bytes_left = 0;
    s_consumed = 0;
    s_no_sync_refills = 0;   /* fresh track: restart sync-word watchdog */
    s_track_errs = 0;        /* fresh track: restart decode-error watchdog */
    s_pcm_stalls = 0;        /* fresh track: restart pipeline-stall watchdog */
    return true;
}

static void close_track(void)
{
    if (s_dec != NULL) {
        MP3FreeDecoder(s_dec);
        s_dec = NULL;
    }
    if (s_src.fp != NULL) {
        fclose(s_src.fp);
        s_src.fp = NULL;
    }
    /* Deliberately NOT parking the I2S channel here: a track switch (next/
     * prev, or list auto-advance) must not stop and restart the clock, or
     * the DMA would clock out stale descriptors and the first write after
     * re-enable would block ~26 ms waiting for the queue. The channel stays
     * enabled across switches; it is parked only when the whole decode loop
     * exits (stop/watchdog), at pause, and by hw_audio_set_player_active
     * itself on the BT route. */
}

/* Rewind the current track for a gapless single-track (repeat-one) loop:
 * seek the source back to byte 0 and REBUILD the helix decoder so the second
 * pass starts from pristine state — a reused decoder keeps bit-reservoir /
 * VBR / resync state that can mis-decode the first frames of the replay. All
 * cursors and watchdogs are reset and the ID3v2 tag is re-skipped (the file
 * may have been re-tagged between passes). Returns false if the seek or the
 * decoder rebuild fails; the caller then falls through to the error path. */
static bool rewind_track(void)
{
    if (fseek(s_src.fp, 0, SEEK_SET) != 0) {
        return false;
    }
    MP3FreeDecoder(s_dec);
    s_dec = MP3InitDecoder();
    if (s_dec == NULL) {
        ESP_LOGE(TAG, "MP3InitDecoder failed on repeat-one rewind");
        return false;
    }
    s_bytes_left = 0;
    s_consumed = 0;
    s_no_sync_refills = 0;   /* fresh pass: restart sync-word watchdog */
    s_track_errs = 0;        /* fresh pass: restart decode-error watchdog */
    s_pcm_stalls = 0;        /* fresh pass: restart pipeline-stall watchdog */
    s_dbg_frames = 0;
    parse_id3v2();           /* re-skip the tag + pick up ReplayGain again */
    hw_audio_set_player_active(true); /* re-arm the pipeline */
    return true;
}

/* Read up to `want` bytes from the active source into `out`. Returns the
 * number of bytes actually read (0 at end of source). */
static int src_read(void *out, int want)
{
    return (int)fread(out, 1, (size_t)want, s_src.fp);
}

/* Decode a single frame and stream it. Returns false on EOF/error. */
static bool decode_frame(bool *rate_set)
{
    if (s_bytes_left < 1024) {
        if (s_consumed > 0) {
            memmove(s_readbuf, s_readbuf + s_consumed, (size_t)s_bytes_left);
            s_consumed = 0;
        }
        int got = src_read(s_readbuf + s_bytes_left,
                           MP3_READ_CHUNK - s_bytes_left);
        s_bytes_left += got;
        if (got == 0 && s_bytes_left < 2) {
            return false;   /* end of file */
        }
    }

    int offset = MP3FindSyncWord(s_readbuf + s_consumed, s_bytes_left);
    if (offset < 0) {
        /* No sync word in buffer: discard and refill next call. */
        ESP_LOGD(TAG, "no sync word in %d buffered bytes, refilling",
                 s_bytes_left);
        s_consumed = 0;
        s_bytes_left = 0;
        if (++s_no_sync_refills >= 64) {
            ESP_LOGE(TAG, "no MP3 sync word after 64 refills, aborting track");
            s_track_errored = true;
            player_report_error(PLAYER_ERR_CORRUPT);
            return false;
        }
        return true;
    }
    s_no_sync_refills = 0;
    s_consumed += offset;
    s_bytes_left -= offset;

    unsigned char *p = s_readbuf + s_consumed;
    int status = MP3Decode(s_dec, &p, &s_bytes_left, s_pcm, 0);
    s_consumed = (int)(p - s_readbuf);

    if (status != 0) {
        /* Skip one byte and resync. Bounded: past TRACK_MAX_DECODE_ERRS the
         * file is treated as corrupt instead of letting the byte-by-byte
         * resync spin the CPU forever on garbage (which also keeps the
         * task-WDT quiet — this is the graceful, fast path). */
        ESP_LOGD(TAG, "MP3Decode status=%d at consumed=%d, resyncing",
                 status, s_consumed);
        if (++s_track_errs >= TRACK_MAX_DECODE_ERRS) {
            ESP_LOGE(TAG, "track '%s' too many decode errors (%d), aborting",
                     s_name, s_track_errs);
            s_track_errored = true;
            player_report_error(PLAYER_ERR_CORRUPT);
            return false;
        }
        if (s_consumed < MP3_READ_CHUNK - 1) {
            s_consumed++;
            s_bytes_left--;
        }
        else {
            s_consumed = 0;
            s_bytes_left = 0;
        }
        return true;
    }
    s_track_errs = 0;

    MP3FrameInfo info;
    MP3GetLastFrameInfo(s_dec, &info);
    if (!*rate_set && info.samprate > 0) {
        hw_audio_set_sample_rate((uint32_t)info.samprate);
        *rate_set = true;
        s_dbg_rate = (uint32_t)info.samprate;
        ESP_LOGD(TAG, "first frame: %u Hz, %d ch, %d kbps, %d samples/frame",
                 (unsigned)info.samprate, info.nChans, info.bitrate,
                 info.outputSamps);
    }

    s_dbg_frames++;
    if (s_dbg_frames % 50 == 0) {
        ESP_LOGD(TAG, "frame #%u consumed=%d out=%d",
                 (unsigned)s_dbg_frames, s_consumed, info.outputSamps);
    }

    audio_write_result_t wr;
    if (info.nChans == 2) {
        wr = hw_audio_write_pcm(s_pcm, (size_t)(info.outputSamps / 2));
    }
    else {
        for (int i = 0; i < info.outputSamps; i++) {
            s_stereo[2 * i] = s_pcm[i];
            s_stereo[2 * i + 1] = s_pcm[i];
        }
        wr = hw_audio_write_pcm(s_stereo, (size_t)info.outputSamps);
    }
    if (wr == AUDIO_WRITE_STALLED) {
        /* The I2S DMA is not consuming (bounded write timed out repeatedly).
         * Give up on the track after a few consecutive stalls — the pipeline
         * is then wedged, and the pipeline error is the honest outcome
         * instead of silent playback forever. */
        if (++s_pcm_stalls >= TRACK_MAX_PIPELINE_STALLS) {
            ESP_LOGE(TAG, "audio pipeline stalled %d times, aborting track",
                     s_pcm_stalls);
            s_track_errored = true;
            player_report_error(PLAYER_ERR_PIPELINE);
            return false;
        }
    }
    else if (wr == AUDIO_WRITE_OK && s_last_err != PLAYER_ERR_NONE) {
        /* First real audio progress of the new track: clear any sticky
         * error from a previously failed track/play request. */
        player_report_error(PLAYER_ERR_NONE);
    }

    return true;
}

static void decode_loop(void)
{
    s_in_decode_loop = true;
    for (;;) {
#if defined(CONFIG_ESP_TASK_WDT_EN)
        /* The task WDT only knows what we feed it (no auto-exemption for
         * blocked tasks in this IDF): reset on every loop pass so a normal
         * track/pause cycle never trips it, while a busy-hang (spinning
         * decode, wedged driver) still fires it after the 5 s timeout. */
        esp_task_wdt_reset();
#endif
        if (s_new_req) {
            s_new_req = false;
            strncpy(s_path, s_new_path, sizeof(s_path) - 1);
            s_path[sizeof(s_path) - 1] = '\0';
            strncpy(s_name, s_new_name, sizeof(s_name) - 1);
            s_name[sizeof(s_name) - 1] = '\0';
            /* Resolve the loaded path back to its list index so the UI can
             * drive next/prev and list-loop works. A path missing from the
             * (possibly stale) list yields -1, which is harmless for
             * index-based navigation and just disables auto-advance. */
            s_index = -1;
            for (int k = 0; k < s_playlist->count; k++) {
                if (strcmp(s_playlist->items[k].path, s_path) == 0) {
                    s_index = k;
                    break;
                }
            }
            /* A pending user stop must survive: player_stop() clears
             * s_new_req, so this block only runs when the request is real. */
            s_stop_req = false;
            s_pause_req = false;
        }
        /* Stop check BEFORE opening the track: a user stop pressed during a
         * track-switch transition (s_new_req was just cleared by player_stop)
         * must not be followed by a pointless open+play of the old request. */
        if (s_stop_req) {
            break;
        }

        s_track_errored = false;
        if (!open_track()) {
            /* Failed to even open the file (deleted since the scan, card
             * hiccup, ...): report and fall through to the shared
             * end-of-track handling below, which auto-advances the list
             * instead of stopping the whole session on one bad file. */
            player_report_error(PLAYER_ERR_OPEN);
            s_track_errored = true;
        }
        else {
            ESP_LOGI(TAG, "start track '%s'", s_name);
            /* 直写模式下无 ring 需预热：首帧解码前即可声明 I2S 归属（通道
             * 直到首个 PCM 帧写入时才真正使能，时钟不会先于数据启动，采样率
             * 也在首帧时立即生效）。旧架构"首帧后才激活"会丢弃每曲首帧，
             * 且与 feed 任务握手存在概率性竞态导致整曲无声。 */
            hw_audio_set_player_active(true);

            bool rate_set = false;
            int frame_cnt = 0;
            s_dbg_frames = 0;             /* reset debug frame counter */
            while (!s_stop_req && !s_new_req) {
                /* Decode-progress heartbeat for the stall watchdog: this
                 * runs once per frame, so while PLAYING a healthy pipeline
                 * keeps it fresh and a task stuck inside decode_frame (SD
                 * read, BT send, ...) lets it go stale. */
                s_decode_beat_ms = (uint32_t)(esp_timer_get_time() / 1000);
#if defined(CONFIG_ESP_TASK_WDT_EN)
                esp_task_wdt_reset();
#endif
                if (s_pause_req) {
                    /* Wait on the REQUEST FLAG, not on the notify alone:
                     * player_play() called from inside this loop (single-track
                     * replay / list auto-advance) self-notifies the task, and
                     * a stale notification would make ulTaskNotifyTake return
                     * pdTRUE instantly and eat the first pause after a track
                     * switch — the player would keep decoding in silence
                     * (frames abandoned) under a PAUSED UI. Consuming the
                     * stale notify here is harmless; only a real resume
                     * (player_toggle clears s_pause_req), a stop, or a new
                     * track request (UI next/prev while paused) exits. The
                     * 2 s timeout still lets the WDT feed run while paused. */
                    while (s_pause_req && !s_stop_req && !s_new_req) {
                        s_decode_beat_ms =
                            (uint32_t)(esp_timer_get_time() / 1000);
#if defined(CONFIG_ESP_TASK_WDT_EN)
                        esp_task_wdt_reset();
#endif
                        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
                    }
                    if (s_stop_req || s_new_req) {
                        break;   /* stop or track switch: end this track */
                    }
                    continue;
                }
                if (!decode_frame(&rate_set)) {
                    break;
                }
                frame_cnt++;
                /* Playback heartbeat (INFO, ~6 s at 44.1 kHz): while a track
                 * plays, healthy decode emits nothing else, so an apparently
                 * frozen serial log is really normal — this line proves the
                 * decode loop is alive and producing frames. */
                if (frame_cnt % 256 == 0) {
                    ESP_LOGI(TAG, "playing... frame #%d (%.1f s)",
                             frame_cnt, (float)frame_cnt * 1152.0f /
                                        (float)(s_dbg_rate ? s_dbg_rate : 44100));
                }
            }

            ESP_LOGI(TAG, "track ended: frames=%u (~%u ms est.), %s",
                     (unsigned)frame_cnt,
                     s_dbg_rate ? (unsigned)((uint64_t)frame_cnt * 1152 /
                                             s_dbg_rate * 1000) : 0,
                     s_stop_req ? "stopped by user"
                                : s_track_errored ? "aborted (error)"
                                                  : "reached EOF");
            /* Single-track loop: replay the SAME file IN PLACE — rewind the
             * source, rebuild the decoder, reset the decode state and start
             * decoding again. The I2S channel is never parked and no request
             * round-trip (which would re-open the file and stop/start the
             * clock) is involved, so the loop is gapless and works even when
             * the track is not in the playlist. Only a clean EOF replays: a
             * corrupt/pipeline failure falls through to the error path. */
            if (!s_stop_req && !s_new_req && !s_track_errored &&
                s_repeat == PLAYER_REPEAT_ONE) {
                if (!rewind_track()) {
                    ESP_LOGE(TAG, "repeat-one rewind failed for '%s'", s_name);
                    s_track_errored = true;
                }
                else {
                    frame_cnt = 0;
                    rate_set = false;
                    ESP_LOGI(TAG, "repeat one: replaying '%s'", s_name);
                    continue;   /* back into the decode while: same file */
                }
            }
            close_track();
        }

        if (s_new_req) {
            continue;   /* switch to the newly requested track */
        }
        if (s_stop_req) {
            break;      /* user stopped (or the watchdog stopped us): end loop */
        }
        /* The track failed (open / corrupt / pipeline): auto-advance so a
         * broken file never wedges the list, skipping the current track even
         * under REPEAT_ONE (replaying a corrupt file would spin forever).
         * Give up after too many consecutive failures so a card full of bad
         * files ends in a visible error instead of a silent cycle. */
        if (s_track_errored) {
            if (++s_fail_count >= TRACK_MAX_CONSEC_FAILS) {
                ESP_LOGE(TAG, "%d consecutive failed tracks, stopping",
                         s_fail_count);
                break;
            }
            int cnt = s_playlist->count;
            if (cnt > 0 && s_index >= 0) {
                int next = (s_index + 1) % cnt;
                player_play(s_playlist->items[next].path);
                continue;
            }
            break;   /* no list to advance to: stop with the error visible */
        }
        s_fail_count = 0;

        /* Natural end of track: advance to the next list entry, wrapping at
         * the end. (Single-track loop never gets here: it replays the file
         * in place inside the track block above.) If the current track isn't
         * in the list (s_index < 0) or the list is empty, just stop. */
        int cnt = s_playlist->count;
        if (cnt <= 0 || s_index < 0) {
            break;
        }
        int next = (s_index + 1) % cnt;
        player_play(s_playlist->items[next].path);
        /* Self-call: only s_new_req is set; the loop top picks up the new
         * track. Re-run rather than break so the next song starts. */
        continue;
    }
    /* Decode loop is leaving for good (stop / watchdog / too many failures):
     * release the I2S bus so the amp powers down and the BT route can take
     * over. (Track switches deliberately do NOT park — see close_track().) */
    hw_audio_set_player_active(false);
    s_state = PLAYER_IDLE;
    s_in_decode_loop = false;
}

static void player_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* Bounded idle wait: the task WDT needs a periodic feed while we sit
         * here (it cannot be fed from a blocked task in this IDF), and a
         * timeout is harmless — the notify that wakes real work comes
         * immediately anyway. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
#if defined(CONFIG_ESP_TASK_WDT_EN)
        esp_task_wdt_reset();
#endif
        if (s_state == PLAYER_PLAYING) {
            decode_loop();
        }
        s_state = PLAYER_IDLE;
    }
}

/* --- Decode stall watchdog --------------------------------------------
 * The decode task can hang inside a blocking call the task WDT cannot see
 * (SD read, Bluetooth send): it just sits there, state stuck at PLAYING,
 * silence forever. This monitor polls the decode heartbeat and, once no
 * frame has been produced for PLAYER_STALL_MS, stops playback cleanly and
 * surfaces PLAYER_ERR_STALL — honest failure instead of a fake-playing
 * device. The decode task itself may still be stuck in a kernel call; when
 * it eventually returns it sees s_stop_req and tears down normally. */
static void player_watch_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        /* Feed the RTC watchdog (see main.c rtc_wdt_arm). This task never
         * blocks on anything but the delay, so while the system is healthy
         * the 30 s reset backstop can never trip; a total CPU freeze (PSRAM
         * bus stall, SD hardware hang) stops the feed and auto-resets. */
        rtc_wdt_feed();
        if (s_state != PLAYER_PLAYING || s_decode_beat_ms == 0) {
            continue;   /* nothing playing / no frame yet: nothing to watch */
        }
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        const uint32_t age = now_ms - s_decode_beat_ms; /* wraps safely */
        if (age < PLAYER_STALL_MS) {
            continue;
        }
        /* Recovery FIRST, log LAST: if the console UART itself is wedged,
         * an ESP_LOG* call blocks forever and the recovery below would never
         * run. The state changes are plain memory stores + one notify, so
         * they always land even when logging is dead. */
        player_report_error(PLAYER_ERR_STALL);
        s_stop_req = true;
        hw_audio_set_player_active(false);   /* release I2S: no fake silence */
        s_state = PLAYER_IDLE;
        if (s_task != NULL) {
            xTaskNotifyGive(s_task);   /* wake the decode loop if it can be */
        }
        ESP_LOGE(TAG, "[WATCHDOG] decode stalled %u ms, stopping playback",
                 (unsigned)age);
    }
}

void player_init(void)
{
    s_state = PLAYER_IDLE;
    s_name[0] = '\0';
    /* Debug tracing is compiled in (LOG_LOCAL_LEVEL) but off by default;
     * the settings page LOG option enables it at runtime. */
    if (xTaskCreate(player_task, "mp3_player", 16 * 1024, NULL, 6, &s_task)
            != pdPASS) {
        ESP_LOGE(TAG, "[ERROR] player task create FAILED");
        s_task = NULL;
    }
    if (xTaskCreate(scan_task, "mp3_scan", 4 * 1024, NULL, 4, &s_scan_task)
            != pdPASS) {
        ESP_LOGE(TAG, "[ERROR] scan task create FAILED");
        s_scan_task = NULL;
    }
    if (xTaskCreate(player_watch_task, "mp3_watch", 4 * 1024, NULL, 2,
                    &s_watch_task) != pdPASS) {
        ESP_LOGE(TAG, "[ERROR] player watchdog task create FAILED");
    }
#if defined(CONFIG_ESP_TASK_WDT_EN)
    /* Last-resort backstop: a decode task busy-hung for 5 s trips the task
     * WDT (it logs the hung task and, depending on config, reboots). The
     * decode loop feeds it on every frame; blocked waits are bounded so a
     * paused player keeps feeding too. The stall watchdog above is the
     * actual recovery path — the WDT only catches what it cannot. */
    if (s_task != NULL) {
        esp_err_t werr = esp_task_wdt_add(s_task);
        if (werr != ESP_OK) {
            ESP_LOGW(TAG, "task WDT subscribe failed: %s",
                     esp_err_to_name(werr));
        }
    }
#endif
    ESP_LOGI(TAG, "[PLAYER] player + scan + watchdog tasks started");
    /* Pre-warm the list (SD is mounted by app_main before player_init). Prefer
     * the on-card cache so a re-entered player shows the list instantly; only
     * fall back to a real scan (which rewrites the cache) when no cache exists.
     * The UI can also force a rebuild via player_rescan(). */
    player_scan_with_cache();
}

/* --- On-card playlist cache -------------------------------------------
 * A flat text file (PLAYER_CACHE_FILE) of TAB-separated records:
 *     <src> <TAB> <title> <TAB> <path> <LF>
 * This avoids pulling in a JSON parser for a list that can be a few hundred
 * entries. Loading the cache is O(n) line reads and is effectively
 * instantaneous versus the FATFS walk that a real scan requires. */

/* Append one entry to the cache file (already open in append mode). The
 * title column is the path's basename, kept for backward compatibility. */
static void playlist_cache_write_entry(FILE *f, const playlist_entry_t *e)
{
    const char *b = strrchr(e->path, '/');
    fprintf(f, "%s\t%s\n", (b != NULL) ? b + 1 : e->path, e->path);
}

/* Serialize the just-published snapshot to the cache file. Called from the
 * scan task after a real scan completes, so a re-entry reads instantly. */
static void playlist_cache_write(const playlist_t *pl)
{
    FILE *f = fopen(PLAYER_CACHE_FILE, "w");
    if (f == NULL) {
        ESP_LOGW(TAG, "cache write failed (open %s)", PLAYER_CACHE_FILE);
        return;
    }
    for (int i = 0; i < pl->count; i++) {
        playlist_cache_write_entry(f, &pl->items[i]);
    }
    fclose(f);
    ESP_LOGI(TAG, "wrote %d entries to cache %s", pl->count, PLAYER_CACHE_FILE);
    /* The RAM snapshot now matches the file: refresh the fingerprint so the
     * next entry/source selection can skip re-reading it. */
    playlist_cache_refresh_fp();
}

/* Parse one cache line into `e`. Returns true on success. `line` is mutated
 * (NUL-terminated at field boundaries). Any malformed line fails the load so
 * a corrupt cache degrades to a real scan instead of a broken list. */
static bool playlist_cache_parse_line(char *line, playlist_entry_t *e)
{
    /* Strip trailing CR/LF. */
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
    }
    if (len == 0) {
        return false;
    }
    char *p_title = line;
    char *p_path = strchr(p_title, '\t');
    if (p_path == NULL) {
        return false;
    }
    *p_path++ = '\0';

    size_t title_len = strlen(p_title);
    if (title_len == 0 || title_len >= MP3_NAME_LEN) {
        return false;
    }
    size_t path_len = strlen(p_path);
    if (path_len == 0 || path_len >= PLAYER_PATH_LEN) {
        return false;
    }
    /* Only the path is stored: the display name is derived from it. The
     * title column is still validated above so a corrupt cache line fails. */
    memcpy(e->path, p_path, path_len + 1);
    return true;
}

/* Load the playlist from the cache file into `work` and publish it. Returns
 * the entry count, or 0 (and publishes an empty list) if the cache is
 * missing/corrupt. A 0-return *may* mean a legitimately empty card, so the
 * caller still triggers a real scan to confirm and (re)write the cache. */
static int playlist_load_from_cache(playlist_t *work)
{
    FILE *f = fopen(PLAYER_CACHE_FILE, "r");
    if (f == NULL) {
        return 0;   /* no cache: caller will scan + write */
    }
    int n = 0;
    char line[PLAYER_PATH_LEN + MP3_NAME_LEN + 32];
    bool corrupt = false;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (n >= PLAYER_SCAN_MAX) {
            break;   /* cache larger than we keep: trust count, skip rest */
        }
        if (!playlist_cache_parse_line(line, &work->items[n])) {
            corrupt = true;
            break;
        }
        n++;
    }
    fclose(f);
    if (corrupt) {
        ESP_LOGW(TAG, "cache corrupt, dropping it");
        unlink(PLAYER_CACHE_FILE);
        return 0;
    }
    /* Cache order is already the sorted order captured at write time; no
     * re-sort needed. Publish as the whole-card folder source. */
    snprintf(s_src_name, sizeof(s_src_name), "整卡");
    playlist_publish(work, n, PL_SRC_FOLDER);
    s_pub_is_whole_card = true;
    playlist_cache_refresh_fp();   /* the snapshot now matches the file */
    ESP_LOGI(TAG, "loaded %d entries from cache %s", n, PLAYER_CACHE_FILE);
    return n;
}

bool player_cache_exists(void)
{
    struct stat st;
    return stat(PLAYER_CACHE_FILE, &st) == 0;
}

void player_scan_with_cache(void)
{
    /* A scan running (or queued) owns the work buffer: loading the cache
     * into it here would write the same snapshot buffer the scan task is
     * concurrently filling and double-publish it — corrupting the list.
     * The scan publishes the same whole-card list anyway (and rewrites the
     * cache), so skipping the cache load is safe; the UI shows "加载中"
     * until the list lands. */
    if (s_scan_busy || s_scan_pending) {
        return;
    }
    /* In-memory snapshot reuse: the published list is already the fresh
     * whole-card list (cache unchanged since it was loaded or written), so
     * a player re-entry needs no SD I/O and no re-publish — no version
     * bump, no UI repaint churn. */
    if (s_pub_is_whole_card && playlist_cache_unchanged()) {
        return;
    }
    playlist_t *work = (s_playlist == &s_pl_a) ? &s_pl_b : &s_pl_a;
    int n = playlist_load_from_cache(work);
    if (n == 0) {
        /* No usable cache: do a real background scan; scan_task writes the
         * cache on completion. Use the whole-card root. */
        ESP_LOGI(TAG, "no cache, scanning SD");
        player_load(PL_SRC_FOLDER, PLAYER_ROOT);
    }
    /* Else: cache published instantly; nothing else to do. */
}

void player_rescan(void)
{
    /* Drop any stale cache first, then force a real scan which rewrites it. */
    unlink(PLAYER_CACHE_FILE);
    ESP_LOGI(TAG, "forced rescan of SD");
    player_load(PL_SRC_FOLDER, PLAYER_ROOT);
}

player_state_t player_state(void)
{
    return s_state;
}

player_repeat_t player_repeat_mode(void)
{
    return s_repeat;
}

void player_repeat_toggle(void)
{
    s_repeat = (s_repeat == PLAYER_REPEAT_ALL) ? PLAYER_REPEAT_ONE
                                               : PLAYER_REPEAT_ALL;
    ESP_LOGI(TAG, "repeat mode -> %s",
             s_repeat == PLAYER_REPEAT_ONE ? "单曲循环" : "列表循环");
}

const char *player_current_name(void)
{
    return s_name;
}

void player_play(const char *path)
{
    /* 原则3：audio 未 ready（I2S/互斥锁未初始化）时不接受播放请求。上报
     * 错误而不是静默忽略，UI 才能告诉用户为什么按播放没反应。 */
    if (!hw_audio_is_ready()) {
        ESP_LOGE(TAG, "[ERROR] play requested but audio not ready, ignored");
        player_report_error(PLAYER_ERR_AUDIO);
        return;
    }
    /* A fresh play from idle is a new attempt by the user: reset the
     * consecutive-failure streak so a recovered player does not immediately
     * give up again. (Auto-advance calls this while PLAYING and must NOT
     * reset the streak — the cap is what stops a corrupt-list cycle.) */
    if (s_state == PLAYER_IDLE) {
        s_fail_count = 0;
    }
    /* `path` is absolute if it begins with '/', else it's a legacy basename
     * resolved under PLAYER_ROOT. Either way the full path is what we open. */
    char full[PLAYER_PATH_LEN];
    if (path[0] == '/') {
        snprintf(full, sizeof(full), "%s", path);
    } else {
        snprintf(full, sizeof(full), PLAYER_ROOT "/%s", path);
    }
    snprintf(s_new_path, sizeof(s_new_path), "%s", full);

    /* Display name = basename of the path. Bounded copy (FATFS LFN caps the
     * leaf name at 255 bytes) so GCC's -Wformat-truncation stays quiet. */
    const char *base = strrchr(full, '/');
    base = (base != NULL) ? base + 1 : full;
    size_t blen = strnlen(base, MP3_NAME_LEN - 1);
    memcpy(s_new_name, base, blen);
    s_new_name[blen] = '\0';

    s_new_req = true;
    /* Interrupt a running track only from outside the decode loop. A
     * self-call (single-track replay / list auto-advance) runs after the
     * track already ended, so it must NOT set s_stop_req: if it did, a user
     * stop pressed in the tiny switch window would be silently cleared by
     * the loop top (player_stop() clears s_new_req, so the pending stop
     * then wins at the loop top's early break). The notify below is ALWAYS
     * sent: it is what wakes a decode loop blocked in the pause wait. */
    if (!s_in_decode_loop) {
        s_stop_req = true;          /* ask any current decode to stop */
    }
    s_state = PLAYER_PLAYING;
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

void player_play_index(int i)
{
    if (i < 0 || i >= player_scan_count()) {
        return;
    }
    /* Play by absolute path from the (immutable) playlist snapshot. The index
     * itself is read-only here — callers may only SELECT an entry, never
     * reorder the list. */
    player_play(player_scan_path(i));
}

int player_current_index(void)
{
    if (s_state == PLAYER_IDLE) {
        return -1;
    }
    return s_index;
}

/* Wrap-around step from the current track: -1 when the list is empty, the
 * target index otherwise. While nothing is loaded the step starts at the
 * first entry, so next/prev still report a sensible cursor position. */
static int player_step(int dir)
{
    const int cnt = player_scan_count();
    if (cnt <= 0) {
        return -1;
    }
    int cur = player_current_index();
    if (cur < 0) {
        cur = 0;
    }
    return (cur + dir + cnt) % cnt;
}

int player_next(void)
{
    const int i = player_step(1);
    if (i >= 0 && s_state != PLAYER_IDLE) {
        player_play_index(i);
    }
    return i;
}

int player_prev(void)
{
    const int i = player_step(-1);
    if (i >= 0 && s_state != PLAYER_IDLE) {
        player_play_index(i);
    }
    return i;
}

void player_toggle(void)
{
    if (s_state == PLAYER_PLAYING) {
        /* Release I2S so any ongoing hw_audio_write_pcm back-pressure loop
         * returns immediately; the decode loop sees s_pause_req next. */
        hw_audio_set_player_active(false);
        s_pause_req = true;
        s_state = PLAYER_PAUSED;
    }
    else if (s_state == PLAYER_PAUSED) {
        s_pause_req = false;
        hw_audio_set_player_active(true);  /* re-acquire I2S bus */
        s_state = PLAYER_PLAYING;
        if (s_task != NULL) {
            xTaskNotifyGive(s_task);       /* wake the decode loop */
        }
    }
}

void player_stop(void)
{
    /*
     * Release the I2S bus FIRST (parks the channel; a concurrent bounded
     * write in hw_audio_write_pcm() bails out via its s_player_active
     * check). Without this, the decode task could keep streaming into a
     * parked/failed DMA and might not see s_stop_req for up to a write
     * timeout (100 ms).
     *
     * The dormant player path (when paused) is woken via the notify below.
     */
    hw_audio_set_player_active(false);
    s_stop_req = true;
    /* A stop is authoritative: cancel any pending play request so a track
     * queued during the stop (e.g. a single-track replay racing the button
     * press) cannot start afterwards. */
    s_new_req = false;
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
    s_state = PLAYER_IDLE;
    /* Forget which track we were on so index-based next/prev (driven from the
     * UI list cursor) starts from a clean base after a stop, instead of the
     * stale whole-card index when we later open a small sub-folder. */
    s_index = -1;
}
