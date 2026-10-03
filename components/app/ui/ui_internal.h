/*
 * Internal shared header for the UI module.
 *
 * ui.c (assembly: screen, menu, battery, key routing, refresh scheduling) and
 * the page files (ui_pages_*.c) share one screen state plus a handful of
 * helpers. This header is the private contract between them — nothing outside
 * components/app/ui includes it.
 *
 * Page files own their own state (selection, scroll window, marquee, ...) and
 * only expose what the assembler needs: build / refresh / key handlers.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"
#include "player.h"     /* MP3_NAME_LEN, player_state_t */
#include "ui.h"
#include "ui_nav.h"
#include "ui_theme.h"
#include "ui_widgets.h"

/* Source-picker capacity (music folders and ebook folders both use it). */
#define SRC_MAX             128

/* ---- Settings row ids ----
 * Shared because ui_state_t sizes its settings row arrays by SETTING_COUNT.
 * Adding an option = appending one id here plus one row to the settings table
 * in ui_pages_settings.c. */
typedef enum {
    SETTING_VOLUME = 0,
    SETTING_MASTER_GAIN,
    SETTING_BACKLIGHT,
    SETTING_BTOUT,
    SETTING_STANDBY,
    SETTING_RESCAN,
    SETTING_RESET,
    SETTING_CLEAR_PROG,
    SETTING_LOG_SAVE,        /* 日志保存：SD 落盘开关 */
    SETTING_LOG_LEVEL,       /* 日志级别：NONE..VERBOSE */
    SETTING_LOG_CLEAR,       /* 清空日志：删除已落盘文件 */
    SETTING_COUNT,
} setting_item_t;

/* Debounced NVS persistence: a change only marks a dirty bit and arms a
 * timer; the flush (run from ui_refresh) commits once the user stops
 * tweaking, folding long-press repeats into a single NVS write. */
#define UI_SETTINGS_SAVE_DELAY_MS   800
#define SETTINGS_DIRTY_VOLUME   (1u << 0)
#define SETTINGS_DIRTY_GAIN     (1u << 1)
#define SETTINGS_DIRTY_BT       (1u << 2)
#define SETTINGS_DIRTY_BACKL    (1u << 3)
#define SETTINGS_DIRTY_STBY     (1u << 4)
#define SETTINGS_DIRTY_LOG      (1u << 5)

/* ---- Marquee (scrolling names that are wider than the row) ---- */
#define LIST_SCROLL_MS      220
#define LIST_SCROLL_GAP     8
#define LIST_LINE_W         (UI_SCREEN_W - 32)
/* Status-bar (pl_prog) text width: the full line minus the 8 px page margins. */
#define PROG_LINE_W         (UI_SCREEN_W - 16)
/* Scratch size: a status-bar "source·track" string is two MP3_NAME_LEN names. */
#define MQ_SRC_LEN          (MP3_NAME_LEN * 2 + 8)

typedef struct {
    int      ofs;           /* current scroll offset, in characters */
    uint32_t at;            /* timestamp of last step (ms) */
    char     src[MQ_SRC_LEN];
    bool     scrolling;
} ui_marquee_t;

/* ---- Screen-wide widget state (owned by ui.c) ---- */
typedef struct {
    lv_obj_t *screen;
    lv_obj_t *page;
    lv_obj_t *title;
    lv_obj_t *value;
    lv_obj_t *sub;
    lv_obj_t *bar;
    lv_obj_t *status;
    lv_obj_t *hint;

    lv_obj_t *menu_page;     /* launcher page container (home screen) */
    lv_obj_t *menu_status;   /* launcher position indicator ([n/4]) */

    lv_obj_t *set_row[SETTING_COUNT];     /* focus fill (settings list) */
    lv_obj_t *set_cursor[SETTING_COUNT];
    lv_obj_t *set_text[SETTING_COUNT];
    lv_obj_t *set_value[SETTING_COUNT];

    /* Shared list views (cursor + text rows, scroll window, focus colours):
     * the music source/track list, the Bluetooth device list and the ebook
     * source/book list. See ui_widgets.h. */
    ui_list_t pl_list;
    lv_obj_t *pl_prog;

    ui_list_t bt_list;
    lv_obj_t *bt_status;

    ui_list_t eb_list;
    lv_obj_t *eb_status;
    lv_obj_t *eb_text_label;
    lv_obj_t *eb_bar;
    lv_obj_t *eb_pct;

    lv_obj_t *battery;   /* battery body outline (persistent gauge) */
    lv_obj_t *bat_cap;   /* positive terminal cap */
    lv_obj_t *bat_seg[5]; /* 5 fill segments inside the battery icon */
    lv_obj_t *bat_text;  /* "100%" label next to the battery */

    lv_obj_t *pl_panel;   /* floating playback control panel (MENU key) */
    lv_obj_t *pl_panel_name;
    lv_obj_t *pl_panel_state;
    lv_obj_t *pl_panel_btn[5];  /* 上一曲 / 播放暂停 / 下一曲 / 停止 / 循环 */

    lv_group_t *group;
} ui_state_t;

extern ui_state_t s_ui;

/* ---- Helpers owned by ui.c ---- */

/* Arm a repaint (the 60 Hz loop paints only when dirty). */
void ui_mark_dirty(void);
/* Show a short-lived message in the hint row (toast). */
void set_action(const char *msg);
/* Hint-row text; a live toast overrides it until it expires. */
void ui_set_hint(const char *normal);
/* Full repaint + synchronous flush (used right after an input). */
void ui_refresh(void);

/* Navigation (see ui_nav.h): push+show, pop+show, show the menu. */
void ui_go(ui_page_t page);
void ui_nav_back_or_menu(void);
void ui_enter_page(ui_page_t page);
void ui_show_menu(void);

/* Text helpers. */
void copy_text(char *dst, size_t dst_size, const char *src);
void copy_utf8_clipped(char *dst, size_t dst_size, const char *src);
const char *strip_ext(const char *name);
int  ui_text_px_width(const char *text);
bool ui_marquee_step(ui_marquee_t *mq, char *out, size_t out_size,
                     int max_w, const char *name);
/* Master volume step (shared by the player page and the settings row). */
void ui_volume_step(int dir);
/* Persist a changed setting (debounced). */
void ui_settings_mark_dirty(uint32_t which);

/* ---- Page: settings ---- */
void ui_settings_load(void);
void ui_settings_flush(void);
void ui_build_settings(lv_obj_t *page);
void ui_refresh_settings(void);
void ui_settings_action(void);           /* A */
void ui_settings_adjust(int step);       /* up/down */
void ui_settings_lr(int dir);            /* left/right */
bool ui_settings_esc(void);              /* B: true if handled inside the page */
void ui_settings_reset_paint(void);

/* ---- Page: music player ---- */
void ui_build_player(lv_obj_t *page);
void ui_refresh_player(void);
void ui_player_action(void);
void ui_player_adjust(int step);
void ui_player_lr(int dir);
void ui_player_select(void);             /* Select: cycle repeat mode */
/* B: returns true when the page handled it internally (view step, stop),
 * false when the caller should pop the navigation stack. */
bool ui_player_esc(void);
void ui_player_reset_paint(void);
void ui_player_notify_sd_change(void);
/* Marquees: advanced every tick by the assembler, pinpoint label updates
 * only — a full repaint per frame would starve the button poller. */
void ui_player_prog_marquee(void);
void ui_player_list_marquee(void);
/* Floating playback panel (MENU key on the ebook pages). */
void ui_player_build_panel(void);
void ui_player_refresh_panel(void);
bool ui_player_panel_open(void);
void ui_player_panel_toggle(void);
/* Returns true when the panel is open and consumed the key. */
bool ui_player_panel_key(uint32_t key);


/* ---- Page: Bluetooth ---- */
void ui_build_bt(lv_obj_t *page);
void ui_refresh_bt(void);
void ui_bt_action(void);
void ui_bt_adjust(int step);
void ui_bt_select(void);                 /* Select: re-scan */
void ui_bt_reset_paint(void);

/* ---- Page: launcher (home screen, replaces the old text menu) ---- */
void ui_build_launcher(void);
void ui_refresh_launcher(void);
void ui_launcher_nav(uint32_t key);     /* UP/DOWN/LEFT/RIGHT over the grid */
ui_page_t ui_launcher_page(void);      /* page of the current selection */
void ui_launcher_reset(void);

/* ---- Page: ebook (list + reader) ---- */
void ui_build_ebook_list(lv_obj_t *page);
void ui_build_ebook_read(lv_obj_t *page);
void ui_refresh_ebook_list(void);
void ui_refresh_ebook_read(void);
void ui_ebook_list_action(void);
void ui_ebook_read_action(void);
void ui_ebook_adjust(int step);
void ui_ebook_lr(int dir);
void ui_ebook_select(void);              /* reader: open the jump overlay */
/* B: true = handled inside the page, false = pop the navigation stack. */
bool ui_ebook_esc(void);
void ui_ebook_reset_paint(void);
void ui_ebook_notify_sd_change(void);
void ui_ebook_list_marquee(void);
