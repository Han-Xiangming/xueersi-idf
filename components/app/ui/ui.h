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
/* BELOW the MP3 decode task (PLAYER_TASK_PRIORITY = 8, see player.c).
 *
 * This ordering was previously the other way round (LVGL 7 above decode 6),
 * which starved the decode task and caused the I2S underrun failure: a 48 kHz
 * frame is only 24 ms of audio, so even a few ms of preemption per frame
 * drains the ~256 ms DMA ring within a second — logged as chronic
 * "I2S write gap > 30 ms" and heard as dropouts / silence. The old comment's
 * assumption ("the decode task tolerates being preempted for one full page
 * render") only holds for occasional renders, not for the continuous refresh
 * of an active player page.
 *
 * Key events and the synchronous render are still responsive: the decode task
 * blocks on I2S DMA back-pressure for most of each frame, so LVGL gets the
 * remaining CPU whenever the decoder is waiting on the hardware. */
#define LVGL_TASK_PRIORITY          7
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
