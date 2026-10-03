/*
 * On-device log sink: StreamBuffer ring + drain task that mirrors every
 * esp_log line to a file on the SD card.
 *
 * Files: per-session, named /sdcard/logs/app_NNNN.log (NNNN resumes across
 * reboots so segments never collide). A ring keeps at most RING_FILES of the
 * newest segments; older ones are deleted so the card can't fill up.
 *
 * Batched writes: lines are accumulated in the libc write buffer (setvbuf)
 * and only flushed + fsync'd when dirty and the drain task goes idle, on
 * rotation, on close, or on an explicit log_sink_flush(). We deliberately do
 * NOT fsync per line: FATFS rewrites the directory entry on every f_sync, so
 * per-line syncing would hammer the SD (wear + latency) for no real benefit —
 * the UART already carries the live copy, and losing a few hundred ms of the
 * SD mirror on a power drop is acceptable.
 *
 * CRITICAL: FATFS only updates a file's directory entry (its on-disk size) on
 * f_close / f_sync. A bare fflush() only flushes the libc buffer down to the
 * VFS; the file would then show as 0 bytes on the card. That is why the sync
 * steps above use fflush() + fsync().
 *
 * Crash capture: this sink is a best-effort mirror. For the *cause* of a crash,
 * enable ESP-IDF core dump to SD/UART (CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH)
 * rather than hand-rolling a panic SD write (the SD/FATFS stack is unsafe in
 * panic context).
 */
#include "log_sink.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"

#include "sd.h"

#define LOG_DIR        "/sdcard/logs"
#define LOG_NAME_PFX   "app_"
#define LOG_NAME_MAX   64
#define RING_FILES     5              /* keep at most this many session logs */
#define STREAM_BYTES   (8 * 1024)
#define LINE_CAP       (512 + 16)     /* must cover XM_LOG_BUF (512) */
#define FILE_CAP       (2 * 1024 * 1024)
#define WRITE_BUF      (4 * 1024)     /* libc write buffer */
#define RECV_MS        500

static StreamBufferHandle_t s_stream;
static FILE *s_fp;
static size_t s_size;
static bool s_active;
static bool s_dirty;          /* unsynced writes since last fsync */
static bool s_enabled = true; /* SD mirror master switch (settings UI) */
static uint32_t s_seq;        /* current segment number */

static void ensure_dir(void)
{
    struct stat st;
    if (stat(LOG_DIR, &st) != 0) {
        mkdir(LOG_DIR, 0755);
    }
}

/* Highest existing segment number in LOG_DIR (0 if none). Lets a new boot
 * resume the sequence so filenames never collide with a previous session. */
static uint32_t scan_max_seq(void)
{
    uint32_t max = 0;
    DIR *d = opendir(LOG_DIR);
    if (d == NULL) {
        return 0;
    }
    struct dirent *e;
    size_t pfx = strlen(LOG_NAME_PFX);
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len < pfx + 4 || strncmp(e->d_name, LOG_NAME_PFX, pfx) != 0) {
            continue;
        }
        if (strcmp(e->d_name + len - 4, ".log") != 0) {
            continue;
        }
        uint32_t v = (uint32_t)strtoul(e->d_name + pfx, NULL, 10);
        if (v > max) {
            max = v;
        }
    }
    closedir(d);
    return max;
}

/* Delete the oldest segment if the ring is full. Called before creating a new
 * segment so the new one is never the one pruned. */
static void prune_ring(void)
{
    uint32_t oldest = 0;
    bool have = false;
    int count = 0;
    DIR *d = opendir(LOG_DIR);
    if (d == NULL) {
        return;
    }
    struct dirent *e;
    size_t pfx = strlen(LOG_NAME_PFX);
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len < pfx + 4 || strncmp(e->d_name, LOG_NAME_PFX, pfx) != 0) {
            continue;
        }
        if (strcmp(e->d_name + len - 4, ".log") != 0) {
            continue;
        }
        uint32_t v = (uint32_t)strtoul(e->d_name + pfx, NULL, 10);
        count++;
        if (!have || v < oldest) {
            oldest = v;
            have = true;
        }
    }
    closedir(d);
    if (count >= RING_FILES && have) {
        char path[LOG_NAME_MAX];
        snprintf(path, sizeof(path), "%s/%s%04" PRIu32 ".log", LOG_DIR, LOG_NAME_PFX, oldest);
        unlink(path);
    }
}

static void fsync_fp(void)
{
    if (s_fp == NULL) {
        return;
    }
    int fd = fileno(s_fp);
    if (fd >= 0) {
        fsync(fd);   /* best-effort; FATFS VFS implements it as f_sync */
    }
}

static void open_file(void)
{
    ensure_dir();
    if (s_seq == 0) {
        s_seq = scan_max_seq() + 1;   /* first segment this boot */
    } else {
        s_seq++;                      /* new segment after rotation */
    }
    char path[LOG_NAME_MAX];
    snprintf(path, sizeof(path), "%s/%s%04" PRIu32 ".log", LOG_DIR, LOG_NAME_PFX, s_seq);
    prune_ring();

    s_fp = fopen(path, "ab");
    if (s_fp == NULL) {
        s_active = false;
        return;
    }
    setvbuf(s_fp, NULL, _IOFBF, WRITE_BUF);
    s_size = (size_t)ftell(s_fp);
    /* Session marker: proves the file opened. If this line is present but the
     * expected logs are not, the enqueue side is broken; if it is absent, the
     * open failed (card not mounted / dir unwritable). */
    const char *base = strrchr(path, '/') + 1;
    fprintf(s_fp, "=== log start: %s ===\n", base);
    fflush(s_fp);
    fsync_fp();
    s_size = (size_t)ftell(s_fp);
    s_dirty = false;
    s_active = true;
}

static void close_file(void)
{
    if (s_fp) {
        fflush(s_fp);
        fsync_fp();
        fclose(s_fp);
        s_fp = NULL;
    }
    s_active = false;
    s_size = 0;
    s_dirty = false;
}

/* Start a fresh segment once the current one grows past FILE_CAP, so a single
 * long session can't fill the card. The ring keeps the newest RING_FILES. */
static void rotate(void)
{
    close_file();
    open_file();
}

static void drain_task(void *arg)
{
    (void)arg;
    uint8_t buf[LINE_CAP];

    if (s_stream == NULL) {
        vTaskDelete(NULL);   /* stream never created: nothing to do */
        return;
    }

    for (;;) {
        if (!hw_sd_is_mounted()) {
            if (s_active) {
                close_file();
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!s_enabled) {
            /* Mirror disabled from settings: don't touch the card at all.
             * Lines still flow to UART (xm_console_vprintf) so the device
             * stays diagnosable over serial; only the SD copy is suppressed. */
            if (s_active) {
                close_file();
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!s_active) {
            open_file();
            if (!s_active) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }

        size_t got = xStreamBufferReceive(s_stream, buf, sizeof(buf),
                                          pdMS_TO_TICKS(RECV_MS));
        if (got == 0) {
            /* Idle: flush + sync only if we actually wrote since the last
             * sync, so an idle drain task never pounds the FAT directory
             * entry. A power drop loses at most ~RECV_MS of buffered lines. */
            if (s_dirty) {
                fflush(s_fp);
                fsync_fp();
                s_dirty = false;
            }
            continue;
        }

        size_t wr = fwrite(buf, 1, got, s_fp);
        if (wr != got) {
            /* Write failed (card error / removed): drop to retry state. */
            close_file();
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        s_size += wr;
        s_dirty = true;   /* synced later (idle / rotate / flush / close) */
        if (s_size >= FILE_CAP) {
            rotate();
        }
    }
}

void log_sink_init(void)
{
    s_stream = xStreamBufferCreate(STREAM_BYTES, 1);
    xTaskCreate(drain_task, "log_sink", 4096, NULL, 1, NULL);
}

/* Force any buffered logs to disk now. Call from error/critical paths so the
 * "incident" lines are durable even if the device then resets. Best-effort. */
void log_sink_flush(void)
{
    if (!s_active || s_fp == NULL) {
        return;
    }
    fflush(s_fp);
    fsync_fp();
    s_dirty = false;
}

/* Called from the IRAM vprintf hook. Non-blocking: copies into the ring
 * buffer and returns immediately; overflow drops the line. */
void log_sink_enqueue(const char *buf, size_t n)
{
    if (s_stream == NULL || n == 0 || !s_enabled) {
        return;
    }
    xStreamBufferSend(s_stream, buf, n, 0);
}

void log_sink_set_enabled(bool on)
{
    s_enabled = on;
    /* Turning off: stop writing right away. Turning on: the drain task opens
     * a fresh segment on its next pass (when the card is mounted). */
    if (!on && s_active) {
        close_file();
    }
}

bool log_sink_get_enabled(void)
{
    return s_enabled;
}

/* Delete every on-card log segment. We close the open file first so unlink
 * never lands on a half-written handle, then reset the sequence so the next
 * open starts at app_0001.log again. The drain task reopens automatically
 * once a new line arrives (and the mirror is still enabled). */
void log_sink_clear(void)
{
    close_file();
    DIR *d = opendir(LOG_DIR);
    if (d == NULL) {
        return;
    }
    struct dirent *e;
    size_t pfx = strlen(LOG_NAME_PFX);
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len < pfx + 4 || strncmp(e->d_name, LOG_NAME_PFX, pfx) != 0) {
            continue;
        }
        if (strcmp(e->d_name + len - 4, ".log") != 0) {
            continue;
        }
        char path[LOG_NAME_MAX];
        snprintf(path, sizeof(path), "%s/%s", LOG_DIR, e->d_name);
        unlink(path);
    }
    closedir(d);
    s_seq = 0;   /* restart the segment sequence at 1 */
}
