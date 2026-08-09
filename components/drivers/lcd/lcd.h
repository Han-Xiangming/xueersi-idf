/*
 * Hardware layer: ST7789 LCD driver and LVGL display binding.
 *
 * Owns the SPI bus, the panel IO handle and the LVGL display object.
 * Exposes a ready-to-use lv_display_t; the software layer must not
 * touch low-level ST7789 commands.
 */
#pragma once

#include <stdbool.h>

#include "board_config.h"
#include "esp_lcd_panel_io.h"
#include "lvgl.h"

/* Initialize SPI bus, panel IO and run the ST7789 init sequence. */
void hw_lcd_init(void);

/* Create and configure the LVGL display (draw buffers + flush binding). */
lv_display_t *hw_lcd_create_display(void);

/* Turn the display on (idempotent). Call after the first LVGL flush. */
void hw_lcd_display_on(void);

/* True once the first frame has been pushed to the panel. */
bool hw_lcd_first_flush_done(void);

/* Set backlight brightness as a percentage (0..100) via PWM on PIN_NUM_LCD_BL. */
void hw_lcd_set_backlight(uint8_t percent);

/* Current backlight brightness as a percentage (0..100), or 0 before the
 * backlight has been initialised. */
uint8_t hw_lcd_get_backlight(void);
