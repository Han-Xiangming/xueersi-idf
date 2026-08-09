/*
 * Software layer: LVGL user interface and application state machine.
 *
 * Consumes the hardware-layer drivers (audio, SD, buttons, LCD) through
 * their public APIs and never touches registers or LVGL display internals.
 */
#pragma once

#include <stdint.h>

#include "board_config.h"
#include "lvgl.h"

/* LVGL service timing / task parameters (application layer). */
#define LVGL_TICK_PERIOD_MS         1
/* Raised from 10 KB: under concurrent BT-A2DP encoding + SDSPI IRQs the LVGL
 * flush path overflowed the 10 KB stack and corrupted the adjacent SPI bus
 * background-lock struct, surfacing as a LoadProhibited panic inside the SPI
 * ISR (spi_bus_lock.c:317). 16 KB leaves headroom for partial-refresh and
 * lv_timer_handler(). */
#define LVGL_TASK_STACK_SIZE        (16 * 1024)
#define LVGL_TASK_PRIORITY          5
#define LVGL_TASK_MIN_DELAY_MS      1
#define LVGL_TASK_MAX_DELAY_MS      16

typedef enum {
    UI_PAGE_PLAYER = 0,
    UI_PAGE_BT,
    UI_PAGE_SETTINGS,
    UI_PAGE_EBOOK_LIST,
    UI_PAGE_EBOOK_READ,
    UI_PAGE_COUNT,
} ui_page_t;

/* Start the LVGL tick timer (1 ms period). */
void ui_start_tick_timer(void);

/* Create the keypad input device and default group. */
lv_group_t *ui_input_init(lv_display_t *display);

/* Build the initial screen and show the first page. */
void ui_create(lv_group_t *group);

/* Periodic UI refresh (called from the main loop). */
void ui_refresh(void);
