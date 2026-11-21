/*
 * Hardware layer: SD card over SDSPI on the shared SPI2 bus (CS=GPIO22).
 *
 * Owns the mount state and exposes it through accessors so the software
 * layer never reaches into card internals.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Called on the task that drives hw_sd_poll(), immediately before FATFS is
 * unmounted. The application must close every open /sdcard file handle here
 * (player track, reader book, background scan/count tasks). */
typedef void (*hw_sd_pre_unmount_cb_t)(void);

void hw_sd_set_pre_unmount_cb(hw_sd_pre_unmount_cb_t cb);

void hw_sd_try_mount(void);
/* Periodic hotplug maintenance: retry mounting when absent, probe the card
 * when mounted and unmount it after repeated probe failures. Call about once
 * per second; must run on a task that may briefly block on SDSPI. */
void hw_sd_poll(void);
/* Tear down the current mount cleanly (runs the pre-unmount callback). */
void hw_sd_unmount(void);

bool hw_sd_is_mounted(void);
const char *hw_sd_name(void);
uint32_t hw_sd_mb(void);
esp_err_t hw_sd_last_err(void);
/* Bumped on every successful mount or unmount. Consumers can compare it to
 * detect a card swap even if removal+insertion happened between two polls. */
uint32_t hw_sd_generation(void);
