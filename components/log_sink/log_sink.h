#pragma once

#include <stddef.h>

/* On-device log sink.
 *
 * Mirrors every esp_log line to a per-session file on the SD card
 * (/sdcard/logs/app_NNNN.log, kept in a ring of RING_FILES newest segments) so
 * the device can be diagnosed without a UART console.
 *
 * Design: the vprintf hook (xm_console_vprintf in main.c) enqueues each
 * formatted line into a FreeRTOS StreamBuffer (non-blocking copy, drops on
 * overflow). A low-priority drain task pops lines and writes them to disk.
 * Writes are batched (libc buffer + setvbuf) and only fsync'd when dirty and
 * idle / on rotation / on close / on log_sink_flush(), to avoid per-line SD
 * wear. This keeps logging entirely off the time-critical decode path.
 *
 * For crash *cause* capture, enable ESP-IDF core dump to SD/UART; do not rely
 * on this sink for panic-time persistence (SD/FATFS is unsafe in panic ctx). */
void log_sink_init(void);
void log_sink_enqueue(const char *buf, size_t n);

/* Flush buffered logs to disk immediately (best-effort). Call from
 * error/critical paths so important lines survive a reset. */
void log_sink_flush(void);
