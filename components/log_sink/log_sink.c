/*
 * On-device log sink: ring-buffer + drain task that writes every esp_log line
 * to /sdcard/logs/app.log. See log_sink.h.
 *
 * CRITICAL: FATFS only updates a file's directory entry (its on-disk size)
 * on f_close / f_sync. A bare fflush() only flushes the libc buffer down to the
 * VFS; the file would then show as 0 bytes on the card even though the data
 * sectors were written. We fsync() after every write (and on open) so the size
 * is always current and a pulled card shows real content.
 */
#include "log_sink.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"

#include "sd.h"

#define LOG_DIR        "/sdcard/logs"
#define LOG_FILE       "/sdcard/logs/app.log"
#define LOG_FILE_BAK   "/sdcard/logs/app.log.bak"
#define STREAM_BYTES   (8 * 1024)
#define LINE_CAP       (512 + 16)      /* must cover XM_LOG_BUF (512) */
#define FILE_CAP       (2 * 1024 * 1024)
#define RECV_MS        500

static StreamBufferHandle_t s_stream;
static FILE *s_fp;
static size_t s_size;
static bool s_active;

static void ensure_dir(void)
{
    struct stat st;
    if (stat(LOG_DIR, &st) != 0) {
        mkdir(LOG_DIR, 0755);
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
    s_fp = fopen(LOG_FILE, "ab");
    if (s_fp == NULL) {
        s_active = false;
        return;
    }
    fseek(s_fp, 0, SEEK_END);
    s_size = (size_t)ftell(s_fp);
    /* Session marker: proves the file opened. If this line is present but the
     * expected logs are not, the enqueue side is broken; if it is absent, the
     * open failed (card not mounted / dir unwritable). */
    fputs("=== log session start ===\n", s_fp);
    fflush(s_fp);
    fsync_fp();
    s_size = (size_t)ftell(s_fp);
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
}

/* Rotate to a single backup once the file grows past FILE_CAP, so the card
 * never fills up from a log flood during a long debugging session. */
static void rotate(void)
{
    close_file();
    rename(LOG_FILE, LOG_FILE_BAK);   /* overwrite previous backup */
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
            /* Idle: flush + sync so a power drop loses at most ~500 ms. */
            fflush(s_fp);
            fsync_fp();
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
        fflush(s_fp);
        fsync_fp();   /* keep the FAT directory entry (file size) current */
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

/* Called from the IRAM vprintf hook. Non-blocking: copies into the ring
 * buffer and returns immediately; overflow drops the line. */
void log_sink_enqueue(const char *buf, size_t n)
{
    if (s_stream == NULL || n == 0) {
        return;
    }
    xStreamBufferSend(s_stream, buf, n, 0);
}
