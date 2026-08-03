/*
 * Hardware layer: 6 active-low key buttons mapped to LVGL keypad keys.
 *
 * Performs GPIO configuration and debounce; exposes the debounced read
 * callback consumed by LVGL's keypad input device.
 */
#pragma once

#include "lvgl.h"

#define BUTTON_ACTIVE_LEVEL         0
#define BUTTON_DEBOUNCE_MS          25

void hw_buttons_init(void);

/* LVGL keypad input-device read callback. */
void hw_buttons_read(lv_indev_t *indev, lv_indev_data_t *data);
