#pragma once

#include <stddef.h>

/* On-device log sink.
 *
 * Mirrors every esp_log line to a file on the SD card (/sdcard/logs/app.log)
 * so the device can be diagnosed without a UART console — which is exactly
 * the situation here: the silent-skip bug is invisible over serial.
 *
 * Design: the vprintf hook (xm_console_vprintf in main.c) enqueues each
 * formatted line into a FreeRTOS StreamBuffer (non-blocking copy, drops on
 * overflow). A low-priority drain task pops lines and writes them to disk.
 * This keeps logging entirely off the time-critical decode path. */
void log_sink_init(void);
void log_sink_enqueue(const char *buf, size_t n);
