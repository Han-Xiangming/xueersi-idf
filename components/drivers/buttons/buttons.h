/*
 * Hardware layer: 9 key buttons (UP/DOWN/LEFT/RIGHT/A/B/SELECT/START/MENU)
 * mapped to LVGL keypad keys.
 *
 * Performs GPIO configuration and debounce; exposes the debounced read
 * callback consumed by LVGL's keypad input device.
 */
#pragma once

#include "lvgl.h"

/* Global media key, used by the UI on every page. This is a CUSTOM key
 * code, NOT LVGL's LV_KEY_NEXT / LV_KEY_PREV: LVGL intercepts those two
 * for group-focus navigation and they never reach the UI's LV_EVENT_KEY
 * handler. (START is deliberately left without a function — it only wakes
 * the screen — so it maps to the inert LV_KEY_END.) */
#define LV_KEY_MEDIA_PANEL 0x101   /* MENU: toggle the floating playback panel */

#define BUTTON_ACTIVE_LEVEL         0
/* Press-debounce time. Kept short (10 ms) because the input is polled every
 * BUTTON_POLL_PERIOD_MS; together they keep press-to-UI latency at ~10-15 ms
 * instead of the ~40 ms a 25 ms debounce on a 16 ms poll would cost. */
#define BUTTON_DEBOUNCE_MS          10
/* LVGL keypad indev poll period: shorter than the 16 ms LVGL refresh period
 * so a press is sampled promptly (applied in ui_input_init). */
#define BUTTON_POLL_PERIOD_MS       5

void hw_buttons_init(void);

/* LVGL keypad input-device read callback. */
void hw_buttons_read(lv_indev_t *indev, lv_indev_data_t *data);
