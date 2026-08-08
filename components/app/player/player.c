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
#include <dirent.h>

#include "mp3dec.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

/* Read buffer for MP3 stream data. */
#define MP3_READ_CHUNK  (4 * 1024)
/* Worst-case decoded PCM: MPEG1 Layer III, 2 channels, 1152 samples/frame. */
#define MP3_PCM_MAX     (2 * 1152 * 2)

static const char *TAG = "player";

static TaskHandle_t s_task;
static player_state_t s_state = PLAYER_IDLE;

static char s_path[PLAYER_PATH_LEN];        /* full path: /sdcard/<name> */
static char s_name[MP3_NAME_LEN];
static bool s_stop_req;
static bool s_pause_req;
static bool s_new_req;
static char s_new_path[PLAYER_PATH_LEN];
static char s_new_name[MP3_NAME_LEN];
static uint32_t s_dbg_frames;          /* decode-frame counter for debug logs */
static uint32_t s_dbg_rate;            /* samplerate captured from 1st frame */

/* Abstract data source: a file on the SD card. */
typedef struct {
    FILE *fp;
} track_src_t;

/* Decoder working buffers (owned by the decode loop). PCM buffers are pure
 * CPU access (decode + mono->stereo copy; hw_audio_write_pcm copies into the
 * PSRAM ring), so they live in PSRAM to keep internal DRAM headroom. */
static track_src_t s_src;
static HMP3Decoder s_dec;
static unsigned char s_readbuf[MP3_READ_CHUNK];
static int s_bytes_left;
static int s_consumed;
EXT_RAM_BSS_ATTR static int16_t s_pcm[MP3_PCM_MAX];
EXT_RAM_BSS_ATTR static int16_t s_stereo[MP3_PCM_MAX];

/* --- Background SD scan -------------------------------------------------
 * opendir/readdir over SDSPI is slow (tens of ms), so the track list is built
 * on its own task. The result is published as a snapshot: the work buffer is
 * filled, then copied into the live array and the version counter bumped.
 * The UI polls the version at its 16 ms cadence, so it never blocks on the
 * scan. EXT_RAM_BSS keeps the two 4 KB lists out of internal DRAM. */
#define PLAYER_SCAN_MAX 64

EXT_RAM_BSS_ATTR static char s_scan_work[PLAYER_SCAN_MAX][MP3_NAME_LEN];
EXT_RAM_BSS_ATTR static char s_scan_names[PLAYER_SCAN_MAX][MP3_NAME_LEN];
static int s_scan_count;
static uint32_t s_scan_version;
static bool s_scan_busy;
static bool s_scan_pending;
static TaskHandle_t s_scan_task;

static int scan_name_cmp(const void *a, const void *b)
{
    return strcasecmp((const char *)a, (const char *)b);
}

/* One directory walk; fills s_scan_work and publishes the snapshot. */
static void scan_do(void)
{
    int n = 0;
    DIR *d = opendir("/sdcard");
    if (d != NULL) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL && n < PLAYER_SCAN_MAX - 1) {
            const char *fn = e->d_name;
            int len = (int)strlen(fn);
            if (len > 4 && strcasecmp(fn + len - 4, ".mp3") == 0) {
                strncpy(s_scan_work[n], fn, MP3_NAME_LEN - 1);
                s_scan_work[n][MP3_NAME_LEN - 1] = '\0';
                n++;
            }
        }
        closedir(d);
    }
    /* readdir order is arbitrary; sort so the list is stable across scans. */
    if (n > 1) {
        qsort(s_scan_work, (size_t)n, MP3_NAME_LEN, scan_name_cmp);
    }
    /* Publish: names first, then count, then version (the UI fetches count
     * before indexing, so it either sees the old list or the new one). */
    memcpy(s_scan_names, s_scan_work, (size_t)n * MP3_NAME_LEN);
    s_scan_count = n;
    s_scan_version++;
}

static void scan_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        s_scan_busy = true;
        do {
            s_scan_pending = false;
            scan_do();
        } while (s_scan_pending);   /* a request landed mid-scan: redo */
        s_scan_busy = false;
    }
}

void player_scan_start(void)
{
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
    return s_scan_names[i];
}

/* Skip an ID3v2 tag at the head of the open file so the MP3 decoder doesn't
 * mistake tag bytes for audio frame headers. An ID3v2 tag starts with the
 * magic "ID3" followed by 7 bytes of header; the 4 size bytes are
 * "syncsafe" (top bit always 0), so the real tag length is:
 *   (b0<<21)|(b1<<14)|(b2<<7)|b3   + 10 header bytes.
 * Without this, MP3FindSyncWord can lock onto a 0xFFEx sequence inside the
 * tag and report a bogus sample rate, wrecking pitch/speed for the track. */
static void skip_id3v2(void)
{
    unsigned char hdr[10];
    long base = ftell(s_src.fp);
    if (fread(hdr, 1, sizeof(hdr), s_src.fp) != sizeof(hdr)) {
        fseek(s_src.fp, base, SEEK_SET);
        return;
    }
    if (hdr[0] != 'I' || hdr[1] != 'D' || hdr[2] != '3') {
        fseek(s_src.fp, base, SEEK_SET);   /* not an ID3v2 tag: rewind */
        return;
    }
    long size = ((long)(hdr[6] & 0x7f) << 21) |
                ((long)(hdr[7] & 0x7f) << 14) |
                ((long)(hdr[8] & 0x7f) << 7)  |
                ((long)(hdr[9] & 0x7f));
    fseek(s_src.fp, base + 10 + size, SEEK_SET);
    ESP_LOGD(TAG, "skipped ID3v2 tag (%ld bytes)", 10L + size);
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
    skip_id3v2();   /* the decoder must only see real audio frames */
    s_bytes_left = 0;
    s_consumed = 0;
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
    hw_audio_set_player_active(false);
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
        return true;
    }
    s_consumed += offset;
    s_bytes_left -= offset;

    unsigned char *p = s_readbuf + s_consumed;
    int status = MP3Decode(s_dec, &p, &s_bytes_left, s_pcm, 0);
    s_consumed = (int)(p - s_readbuf);

    if (status != 0) {
        /* Skip one byte and resync. */
        ESP_LOGD(TAG, "MP3Decode status=%d at consumed=%d, resyncing",
                 status, s_consumed);
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

    if (info.nChans == 2) {
        hw_audio_write_pcm(s_pcm, (size_t)(info.outputSamps / 2));
    }
    else {
        for (int i = 0; i < info.outputSamps; i++) {
            s_stereo[2 * i] = s_pcm[i];
            s_stereo[2 * i + 1] = s_pcm[i];
        }
        hw_audio_write_pcm(s_stereo, (size_t)info.outputSamps);
    }

    return true;
}

static void decode_loop(void)
{
    for (;;) {
        if (s_new_req) {
            s_new_req = false;
            strncpy(s_path, s_new_path, sizeof(s_path) - 1);
            s_path[sizeof(s_path) - 1] = '\0';
            strncpy(s_name, s_new_name, sizeof(s_name) - 1);
            s_name[sizeof(s_name) - 1] = '\0';
            s_stop_req = false;
            s_pause_req = false;
        }

        if (!open_track()) {
            s_state = PLAYER_IDLE;
            return;
        }
        hw_audio_set_player_active(true);
        ESP_LOGI(TAG, "start track '%s'", s_name);

        bool rate_set = false;
        int frame_cnt = 0;
        s_dbg_frames = 0;                 /* reset debug frame counter */
        while (!s_stop_req) {
            if (s_pause_req) {
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                s_pause_req = false;
                if (s_stop_req) {
                    break;
                }
                continue;
            }
            if (!decode_frame(&rate_set)) {
                break;
            }
            frame_cnt++;
        }

        ESP_LOGI(TAG, "track ended: frames=%u (~%u ms est.), %s",
                 (unsigned)frame_cnt,
                 s_dbg_rate ? (unsigned)((uint64_t)frame_cnt * 1152 /
                                         s_dbg_rate * 1000) : 0,
                 s_stop_req ? "stopped by user" : "reached EOF");
        close_track();

        if (s_new_req) {
            continue;   /* switch to the newly requested track */
        }
        break;
    }
    s_state = PLAYER_IDLE;
}

static void player_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_state == PLAYER_PLAYING) {
            decode_loop();
        }
        s_state = PLAYER_IDLE;
    }
}

void player_init(void)
{
    s_state = PLAYER_IDLE;
    s_name[0] = '\0';
    /* Debug tracing is compiled in (LOG_LOCAL_LEVEL) but off by default;
     * the settings page LOG option enables it at runtime. */
    xTaskCreate(player_task, "mp3_player", 16 * 1024, NULL, 5, &s_task);
    xTaskCreate(scan_task, "mp3_scan", 4 * 1024, NULL, 4, &s_scan_task);
    /* Pre-warm the list (SD is mounted by app_main before player_init). The
     * UI re-requests scans on page entry / SD hotplug. */
    player_scan_start();
}

player_state_t player_state(void)
{
    return s_state;
}

const char *player_current_name(void)
{
    return s_name;
}

void player_play(const char *name)
{
    /* Clamp the track name to MP3_NAME_LEN so the "/sdcard/<name>" path can
     * never exceed PLAYER_PATH_LEN (strlen("/sdcard/") + MP3_NAME_LEN). */
    size_t nlen = strnlen(name, MP3_NAME_LEN + 1);
    if (nlen > MP3_NAME_LEN) {
        ESP_LOGW(TAG, "name too long (%u), truncating", (unsigned)nlen);
    }
    strncpy(s_new_name, name, sizeof(s_new_name) - 1);
    s_new_name[sizeof(s_new_name) - 1] = '\0';
    snprintf(s_new_path, sizeof(s_new_path), "/sdcard/%s", s_new_name);

    s_new_req = true;
    s_stop_req = true;          /* ask any current decode to stop */
    s_state = PLAYER_PLAYING;
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
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
     * Release the I2S bus FIRST so the decode task's back-pressure loop
     * inside hw_audio_write_pcm() bails out immediately (it checks
     * s_player_active before and during every write). Without this, the
     * decode task stays stuck waiting for I2S to drain the ring and might
     * not see s_stop_req for hundreds of milliseconds.
     *
     * The dormant player path (when paused) is woken via the notify below.
     */
    hw_audio_set_player_active(false);
    s_stop_req = true;
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
    s_state = PLAYER_IDLE;
}
