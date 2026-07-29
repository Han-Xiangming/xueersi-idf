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

void hw_sd_try_mount(void);
void hw_sd_unmount(void);

bool hw_sd_is_mounted(void);
const char *hw_sd_name(void);
uint32_t hw_sd_mb(void);
esp_err_t hw_sd_last_err(void);
