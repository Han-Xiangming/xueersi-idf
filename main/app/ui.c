/*
 * Software layer: LVGL UI and application logic.
 * See ui.h.
 */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/param.h>

#include "board_config.h"
#include "hardware/buttons.h"
#include "hardware/audio.h"
#include "hardware/bt_audio.h"
#include "hardware/sd.h"
#include "app/ui.h"
#include "player.h"
#include "ebook.h"

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "lvgl.h"
#include "sdkconfig.h"
#include "nvs_flash.h"
#include "nvs.h"

/* Embedded CJK bitmap font (main/fonts/lv_font_cn_12.c): ~810 KB, covers
 * ASCII + ~22000 Chinese ideographs + Japanese kana/kanji + CJK symbols.
 * Generated with lv_font_conv from SourceHanSansSC. Used as the UI default
 * so both static labels and dynamic text (SD/BT names) render Chinese. */
extern const lv_font_t lv_font_cn_12;

/* Single UI font: the CJK font includes Latin glyphs too, so there is no
 * need to mix multiple faces. */
#define UI_FONT (&lv_font_cn_12)

#define UI_ACTION_MSG_MS            850
/* Delay before persisting a changed setting to NVS, so a burst of key
 * repeats collapses into a single write. */
#define UI_SETTINGS_SAVE_DELAY_MS   800

static const char *const s_page_names[UI_PAGE_COUNT] = {
    "MP3 Player",
    "SD卡",
    "蓝牙",
    "设置",
    "电子书",
    "电子书",
};

/* Main menu lists only these pages. Bluetooth was moved into Settings as a
 * sub-page (opened from the 蓝牙 settings item), so it is no longer a
 * top-level tab in the main menu. */
static const ui_page_t s_menu_pages[] = {
    UI_PAGE_PLAYER, UI_PAGE_EBOOK_LIST, UI_PAGE_SD, UI_PAGE_SETTINGS,
};
#define UI_MENU_PAGE_COUNT ((int)(sizeof(s_menu_pages) / sizeof(s_menu_pages[0])))

/* Settings sub-menu: up/down to select an item, left/right to change it.
 * Add new options here and they appear automatically in the list. */
typedef enum {
    SETTING_VOLUME = 0,
    SETTING_BTOUT,
    SETTING_LOG,
    SETTING_COUNT,
} setting_item_t;

static const int s_setting_y[SETTING_COUNT] = {30, 50, 70};

/* Verbose (DEBUG) logging; off = normal INFO. Covers the audio/player tags
 * and the Bluetooth stack (our bt_audio wrapper plus the classic-BT Bluedroid
 * components that surface discovery / connection internals). */
static bool s_log_debug;

/* Bluetooth output master switch (settings page ON/OFF). Persisted to NVS;
 * restored at boot. Drives bt_audio_set_enabled() — the audio routing gate. */
static bool s_bt_on;

static void ui_apply_log_level(void)
{
    esp_log_level_t lvl = s_log_debug ? ESP_LOG_DEBUG : ESP_LOG_INFO;
    esp_log_level_set("player", lvl);
    esp_log_level_set("hw_audio", lvl);
    esp_log_level_set("bt_audio", lvl);
}

/* Settings persistence: volume and log level survive reboot via NVS.
 * NVS is initialised in app_main() before ui_create(), so these helpers
 * can open the handle at any time. */
#define UI_NVS_NS      "ui_cfg"
#define UI_NVS_VOLUME  "volume"
#define UI_NVS_VOLBT   "vol_bt"
#define UI_NVS_LOGDBG  "log_dbg"
#define UI_NVS_BT      "bt_on"

static void ui_settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open(UI_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    int32_t v = -1;
    if (nvs_get_i32(h, UI_NVS_VOLUME, &v) == ESP_OK && v >= 0 && v <= 100) {
        hw_audio_set_speaker_volume((uint8_t)v);
    }
    v = -1;
    if (nvs_get_i32(h, UI_NVS_VOLBT, &v) == ESP_OK && v >= 0 && v <= 100) {
        hw_audio_set_bt_volume((uint8_t)v);
    }
    int32_t dbg = 0;
    if (nvs_get_i32(h, UI_NVS_LOGDBG, &dbg) == ESP_OK) {
        s_log_debug = (dbg != 0);
        ui_apply_log_level();
    }
    int32_t bt = 0;
    if (nvs_get_i32(h, UI_NVS_BT, &bt) == ESP_OK) {
        s_bt_on = (bt != 0);
        bt_audio_set_enabled(s_bt_on);
    }
    nvs_close(h);
}

static void ui_settings_save_volume(void)
{
    nvs_handle_t h;
    if (nvs_open(UI_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, UI_NVS_VOLUME, (int32_t)hw_audio_get_speaker_volume());
        nvs_set_i32(h, UI_NVS_VOLBT, (int32_t)hw_audio_get_bt_volume());
        nvs_commit(h);
        nvs_close(h);
    }
}

static void ui_settings_save_log(void)
{
    nvs_handle_t h;
    if (nvs_open(UI_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, UI_NVS_LOGDBG, s_log_debug ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void ui_settings_save_bt(void)
{
    nvs_handle_t h;
    if (nvs_open(UI_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, UI_NVS_BT, s_bt_on ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* Debounced persistence: a setting change only marks a dirty bit and arms a
 * timer; ui_settings_flush() (called every refresh) commits once the user
 * stops tweaking, folding long-press repeats into a single NVS write. */
#define SETTINGS_DIRTY_VOLUME   (1u << 0)
#define SETTINGS_DIRTY_LOG      (1u << 1)
#define SETTINGS_DIRTY_BT       (1u << 2)

static uint32_t s_save_pending;
static uint32_t s_save_at_ms;

static void ui_settings_mark_dirty(uint32_t which)
{
    s_save_pending |= which;
    s_save_at_ms = lv_tick_get() + UI_SETTINGS_SAVE_DELAY_MS;
}

static void ui_settings_flush(void)
{
    if (s_save_pending == 0) {
        return;
    }
    if ((int32_t)(s_save_at_ms - lv_tick_get()) > 0) {
        return;
    }
    if (s_save_pending & SETTINGS_DIRTY_VOLUME) {
        ui_settings_save_volume();
    }
    if (s_save_pending & SETTINGS_DIRTY_LOG) {
        ui_settings_save_log();
    }
    if (s_save_pending & SETTINGS_DIRTY_BT) {
        ui_settings_save_bt();
    }
    s_save_pending = 0;
}

static int s_setting_sel = 0;

#define MP3_LIST_ROWS 4
static const int s_pl_row_y[MP3_LIST_ROWS] = {22, 40, 58, 76};

/* Bluetooth sink picker: same 4-row list layout as the MP3 page. */
#define BT_LIST_ROWS 4
static const int s_bt_row_y[BT_LIST_ROWS] = {22, 40, 58, 76};
static int s_bt_sel;
/* Snapshot of the BT device list so the UI does not re-format device names
 * (incl. the MAC-address fallback) on every 16 ms tick. Refreshed only when
 * bt_audio_device_version() advances. */
static uint32_t s_bt_list_ver;
static int      s_bt_list_cnt;
static char     s_bt_list_name[BT_MAX_DEVICES][BT_DEV_NAME_LEN];
/* 4 KB of song names is UI-only data: keep it in external PSRAM so it does
 * not compete with the Bluetooth stack for internal DRAM. */
static int s_mp3_count;
static int s_mp3_sel;

/* Ebook book-list page: same 4-row layout as the MP3 page. */
#define EBOOK_LIST_ROWS 4
static const int s_eb_row_y[EBOOK_LIST_ROWS] = {22, 40, 58, 76};
static int s_eb_sel;
static char s_eb_open_name[MP3_NAME_LEN];

/* BIOS/DOS-style menu palette: dark base + cyan accent + gray monochrome text. */
static const uint32_t UI_CYAN = 0x00E0E0;
static const uint32_t UI_GRAY = 0x808080;
static const uint32_t UI_BG_DARK = 0x000000;
static const uint32_t UI_TITLE = 0xFF8000;   /* title-bar accent (orange) */

/* Main-menu layout: up to 5 list rows (first UI_MENU_PAGE_COUNT are active). */
#define UI_MENU_ROWS 5
static const int s_menu_y[UI_MENU_ROWS] = {22, 42, 62, 82, 102};

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *page;
    lv_obj_t *title;
    lv_obj_t *value;
    lv_obj_t *sub;
    lv_obj_t *bar;
    lv_obj_t *status;
    lv_obj_t *hint;

    lv_obj_t *menu_page;
    lv_obj_t *menu_cursor[UI_MENU_ROWS];
    lv_obj_t *menu_text[UI_MENU_ROWS];
    lv_obj_t *menu_status;

    lv_obj_t *set_cursor[SETTING_COUNT];
    lv_obj_t *set_text[SETTING_COUNT];
    lv_obj_t *set_value[SETTING_COUNT];

    lv_obj_t *pl_cursor[MP3_LIST_ROWS];
    lv_obj_t *pl_text[MP3_LIST_ROWS];
    lv_obj_t *pl_prog;

    lv_obj_t *bt_cursor[BT_LIST_ROWS];
    lv_obj_t *bt_text[BT_LIST_ROWS];
    lv_obj_t *bt_status;

    lv_obj_t *eb_cursor[EBOOK_LIST_ROWS];
    lv_obj_t *eb_text[EBOOK_LIST_ROWS];
    lv_obj_t *eb_status;
    lv_obj_t *eb_text_label;
    lv_obj_t *eb_bar;
    lv_obj_t *eb_pct;

    lv_group_t *group;
    ui_page_t page_id;
} ui_state_t;

static ui_state_t s_ui;
static bool s_in_menu;
static int s_menu_sel;
static uint32_t s_action_until_ms;
static char s_action[32];

/* On-change refresh (optimization #4): ui_refresh() recomputes the current
 * page only when this is set, then clears it. Input handlers and the engine
 * watcher arm it, turning the 60 Hz tick into an on-change refresh. */
static bool s_ui_dirty = true;

/* Selection-highlight guard (optimization #1): restyle list rows only when the
 * cursor actually moves. These remember the painted selection (and, for the
 * player list, the scroll window) so a static tick skips the style churn.
 * -1 forces a repaint right after a page is (re)built. */
static int s_paint_set_sel  = -1;
static int s_paint_mp3_sel  = -1;
static int s_paint_mp3_top  = -1;
static int s_paint_bt_sel   = -1;
static int s_paint_eb_sel   = -1;

/* Cached async state so ui_refresh() can detect engine-side changes (Bluetooth
 * stack / player / SD) that arrive via callbacks or hardware rather than UI
 * input. */
static uint32_t        s_ext_bt_ver;
static int             s_ext_bt_count;
static bt_pair_state_t s_ext_bt_pair  = BT_PAIR_IDLE;
static bool            s_ext_bt_conn;
static bool            s_ext_bt_scan;
static bool            s_ext_sd_mounted;
static uint32_t        s_ext_scan_ver;    /* player's MP3 list refresh */
static player_state_t  s_ext_pl_state = PLAYER_IDLE;
static char            s_ext_pl_name[MP3_NAME_LEN];
static uint32_t        s_ext_eb_scan_ver;
static uint32_t        s_ext_eb_cnt_ver;

static void ui_mark_dirty(void)
{
    s_ui_dirty = true;
}

static void copy_text(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) {
        return;
    }
    snprintf(dst, dst_size, "%s", src ? src : "");
}

static void set_action(const char *msg)
{
    copy_text(s_action, sizeof(s_action), msg);
    /* The toast shows immediately on the next refresh and stays up for
     * UI_ACTION_MSG_MS. Rapid repeats (key auto-repeat) only update the text
     * and extend the expiry. */
    s_action_until_ms = lv_tick_get() + UI_ACTION_MSG_MS;
    s_ui_dirty = true;                 /* refresh until the toast expires */
}

/* Set a label's text only when it actually differs from what is already
 * shown. ui_refresh() runs every tick (~60 Hz); without this guard the same
 * string would be pushed into LVGL (and re-formatted) over and over, churning
 * the label's text buffer and dirty-rectangle bookkeeping for no reason. */
static void ui_label_set(lv_obj_t *label, const char *text)
{
    if (label == NULL || text == NULL) {
        return;
    }
    const char *cur = lv_label_get_text(label);
    if (cur != NULL && strcmp(cur, text) == 0) {
        return;                       /* unchanged — skip the LVGL set */
    }
    lv_label_set_text(label, text);
}

/* Step the master volume by `dir` (+1/-1) with fine control near silence:
 * below 10% the step is 1% (quiet speech is very sensitive there), above it
 * the given coarse step applies. Boundary cases resolve to the fine step so
 * e.g. 10% - coarse lands on 9%, not 0%/5%. */
static void ui_volume_step(int dir, int coarse)
{
    int v = (int)hw_audio_get_volume();
    int step = (v < 10 || (v == 10 && dir < 0)) ? 1 : coarse;
    v += dir * step;
    /* Snap coarse upward moves onto multiples of the coarse step once out of
     * the fine zone (9% + coarse -> 10%, keeps the scale tidy). */
    if (step == 1 && dir > 0 && v > 10) {
        v = 10;
    }
    hw_audio_set_volume((uint8_t)MAX(0, MIN(v, 100)));
}

static const char *short_err(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return "OK";
    case ESP_ERR_TIMEOUT:
        return "TIMEOUT";
    case ESP_ERR_NOT_FOUND:
        return "NOT FOUND";
    case ESP_ERR_INVALID_STATE:
        return "STATE";
    case ESP_ERR_INVALID_ARG:
        return "ARG";
    case ESP_FAIL:
        return "FAIL";
    default:
        return "ERR";
    }
}

static lv_obj_t *ui_label(lv_obj_t *parent,
                          const char *text,
                          int y,
                          uint32_t color,
                          const lv_font_t *font,
                          lv_text_align_t align)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_size(label, LCD_H_RES - 16, LV_SIZE_CONTENT);
    lv_obj_set_pos(label, 8, y);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_align(label, align, 0);
    return label;
}

static lv_obj_t *ui_make_page(int x)
{
    lv_obj_t *page = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(page);
    lv_obj_set_pos(page, x, 0);
    lv_obj_set_size(page, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_color(page, lv_color_hex(UI_BG_DARK), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    return page;
}

static lv_obj_t *ui_bar(lv_obj_t *parent, int value)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, 18, 86);
    lv_obj_set_size(bar, 124, 8);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, value, LV_ANIM_OFF);
    lv_obj_set_style_radius(bar, 4, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_GRAY), 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_CYAN), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
    return bar;
}

static void ui_set_bar(int value)
{
    if (s_ui.bar) {
        lv_bar_set_value(s_ui.bar, MAX(0, MIN(value, 100)), LV_ANIM_ON);
    }
}

static void ui_set_hint(const char *normal)
{
    if (!s_ui.hint) {
        return;
    }
    const uint32_t now_ms = lv_tick_get();
    if (s_action_until_ms && (int32_t)(s_action_until_ms - now_ms) > 0) {
        ui_label_set(s_ui.hint, s_action);
        /* Toast takes over the status row: hide the ebook bar/percentage
         * so the centered toast text does not collide with them. */
        if (s_ui.eb_bar && s_ui.eb_pct) {
            lv_obj_add_flag(s_ui.eb_bar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_ui.eb_pct, LV_OBJ_FLAG_HIDDEN);
        }
    }
    else {
        s_action_until_ms = 0;         /* toast expired: arm the next one afresh */
        ui_label_set(s_ui.hint, normal);
        if (s_ui.eb_bar && s_ui.eb_pct) {
            lv_obj_remove_flag(s_ui.eb_bar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_ui.eb_pct, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void ui_build_settings(lv_obj_t *page)
{
    s_setting_sel = 0;
    static const char *const labels[SETTING_COUNT] = {"音量", "蓝牙", "日志等级"};

    for (int i = 0; i < SETTING_COUNT; i++) {
        lv_obj_t *cur = lv_label_create(page);
        lv_label_set_text(cur, " ");
        lv_obj_set_pos(cur, 8, s_setting_y[i]);
        lv_obj_set_style_text_font(cur, &lv_font_cn_12, 0);
        lv_obj_set_style_text_color(cur, lv_color_hex(UI_GRAY), 0);
        s_ui.set_cursor[i] = cur;

        /* Fixed left-aligned label; the live value lives in its own column
         * (set_value) so items with different label lengths still line up
         * regardless of the (non-monospaced) byte width of the UTF-8 text. */
        lv_obj_t *txt = lv_label_create(page);
        lv_label_set_text(txt, labels[i]);
        lv_obj_set_pos(txt, 18, s_setting_y[i]);
        lv_obj_set_style_text_font(txt, &lv_font_cn_12, 0);
        lv_obj_set_style_text_color(txt, lv_color_hex(UI_GRAY), 0);
        s_ui.set_text[i] = txt;

        lv_obj_t *val = lv_label_create(page);
        lv_label_set_long_mode(val, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_pos(val, 95, s_setting_y[i]);
        lv_obj_set_style_text_font(val, &lv_font_cn_12, 0);
        lv_obj_set_style_text_color(val, lv_color_hex(UI_GRAY), 0);
        s_ui.set_value[i] = val;
    }

    s_ui.hint = ui_label(page, "上/下选 A进入 左/右设 B返回", 110,
                         UI_GRAY, &lv_font_cn_12, LV_TEXT_ALIGN_CENTER);
}

static void ui_build_player(lv_obj_t *page)
{
    s_mp3_sel = 0;
    s_mp3_count = player_scan_count();
    player_scan_start();   /* refresh the list in the background */

    for (int i = 0; i < MP3_LIST_ROWS; i++) {
        lv_obj_t *cur = lv_label_create(page);
        lv_label_set_text(cur, " ");
        lv_obj_set_pos(cur, 6, s_pl_row_y[i]);
        lv_obj_set_style_text_font(cur, &lv_font_cn_12, 0);
        lv_obj_set_style_text_color(cur, lv_color_hex(UI_CYAN), 0);
        s_ui.pl_cursor[i] = cur;

        lv_obj_t *txt = lv_label_create(page);
        lv_label_set_long_mode(txt, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_pos(txt, 16, s_pl_row_y[i]);
        lv_obj_set_style_text_font(txt, &lv_font_cn_12, 0);
        lv_obj_set_style_text_color(txt, lv_color_hex(UI_GRAY), 0);
        s_ui.pl_text[i] = txt;
    }

    s_ui.pl_prog = ui_label(page, "空闲", 92, UI_GRAY, &lv_font_cn_12, LV_TEXT_ALIGN_CENTER);
    s_ui.hint = ui_label(page, "上/下选择 A播放 B返回", 110, UI_GRAY, &lv_font_cn_12, LV_TEXT_ALIGN_CENTER);
}

/* Bluetooth page: entering it kicks off a scan; the list fills live. */
static void ui_build_bt(lv_obj_t *page)
{
    s_bt_sel = 0;

    for (int i = 0; i < BT_LIST_ROWS; i++) {
        lv_obj_t *cur = lv_label_create(page);
        lv_label_set_text(cur, " ");
        lv_obj_set_pos(cur, 6, s_bt_row_y[i]);
        lv_obj_set_style_text_font(cur, &lv_font_cn_12, 0);
        lv_obj_set_style_text_color(cur, lv_color_hex(UI_CYAN), 0);
        s_ui.bt_cursor[i] = cur;

        lv_obj_t *txt = lv_label_create(page);
        lv_label_set_long_mode(txt, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_pos(txt, 16, s_bt_row_y[i]);
        lv_obj_set_style_text_font(txt, &lv_font_cn_12, 0);
        lv_obj_set_style_text_color(txt, lv_color_hex(UI_GRAY), 0);
        s_ui.bt_text[i] = txt;
    }

    s_ui.bt_status = ui_label(page, "扫描中...", 92, UI_GRAY,
                              &lv_font_cn_12, LV_TEXT_ALIGN_CENTER);
    s_ui.hint = ui_label(page, "上/下选 A连接 B返回", 110, UI_GRAY,
                         &lv_font_cn_12, LV_TEXT_ALIGN_CENTER);

    bt_audio_scan_start();
}

/* Book display name: drop the trailing ".txt" extension. */
static void strip_txt_ext(char *dst, size_t dst_size, const char *src)
{
    snprintf(dst, dst_size, "%s", src ? src : "");
    size_t l = strlen(dst);
    if (l > 4 && strcasecmp(dst + l - 4, ".txt") == 0) {
        dst[l - 4] = '\0';
    }
}

static void ui_build_ebook_list(lv_obj_t *page)
{
    s_eb_sel = 0;

    for (int i = 0; i < EBOOK_LIST_ROWS; i++) {
        lv_obj_t *cur = lv_label_create(page);
        lv_label_set_text(cur, " ");
        lv_obj_set_pos(cur, 6, s_eb_row_y[i]);
        lv_obj_set_style_text_font(cur, &lv_font_cn_12, 0);
        lv_obj_set_style_text_color(cur, lv_color_hex(UI_CYAN), 0);
        s_ui.eb_cursor[i] = cur;

        lv_obj_t *txt = lv_label_create(page);
        lv_label_set_long_mode(txt, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_pos(txt, 16, s_eb_row_y[i]);
        lv_obj_set_style_text_font(txt, &lv_font_cn_12, 0);
        lv_obj_set_style_text_color(txt, lv_color_hex(UI_GRAY), 0);
        s_ui.eb_text[i] = txt;
    }

    s_ui.eb_status = ui_label(page, "扫描中...", 92, UI_GRAY,
                              &lv_font_cn_12, LV_TEXT_ALIGN_CENTER);
    s_ui.hint = ui_label(page, "上/下选 A打开 B返回", 110, UI_GRAY,
                         &lv_font_cn_12, LV_TEXT_ALIGN_CENTER);

    ebook_scan_start();
}

static void ui_build_ebook_read(lv_obj_t *page)
{
    /* Single body label: the reader engine joins exactly 5 lines with '\n'
     * and measures with the same font, so the layout matches exactly. Line
     * space -7 compresses the font's 23 px line height to 16 px rows. The
     * widget must be 4*16 + 23 = 87 px tall (y=22..109), otherwise the last
     * line's 7 px below-baseline box (descenders) is clipped. */
    lv_obj_t *txt = lv_label_create(page);
    lv_label_set_long_mode(txt, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_pos(txt, 8, 22);
    lv_obj_set_size(txt, 152, 87);
    lv_obj_set_style_text_font(txt, &lv_font_cn_12, 0);
    lv_obj_set_style_text_line_space(txt, -7, 0);
    lv_obj_set_style_text_color(txt, lv_color_hex(UI_GRAY), 0);
    s_ui.eb_text_label = txt;

    /* Status row: wide text progress bar at the left (15 cells, ~110 px) and
     * the percentage right-aligned to the 152 px right edge. The centered
     * hint label below is used only by toasts (see ui_set_hint). */
    lv_obj_t *bar = lv_label_create(page);
    lv_label_set_text(bar, "[---------------]");
    lv_label_set_long_mode(bar, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_pos(bar, 8, 111);
    lv_obj_set_style_text_font(bar, &lv_font_cn_12, 0);
    lv_obj_set_style_text_color(bar, lv_color_hex(UI_GRAY), 0);
    s_ui.eb_bar = bar;

    lv_obj_t *pct = lv_label_create(page);
    lv_label_set_text(pct, "0%");
    lv_label_set_long_mode(pct, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_pos(pct, 120, 111);
    lv_obj_set_size(pct, 32, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(pct, &lv_font_cn_12, 0);
    lv_obj_set_style_text_color(pct, lv_color_hex(UI_GRAY), 0);
    lv_obj_set_style_text_align(pct, LV_TEXT_ALIGN_RIGHT, 0);
    s_ui.eb_pct = pct;

    s_ui.hint = ui_label(page, "", 111, UI_GRAY, &lv_font_cn_12,
                         LV_TEXT_ALIGN_CENTER);
}

static void ui_build_page_content(lv_obj_t *page)
{
    s_ui.title = ui_label(page, s_page_names[s_ui.page_id], 2, UI_TITLE, &lv_font_cn_12, LV_TEXT_ALIGN_LEFT);
    /* Top-right status label. Secondary pages leave it empty; the Player page
     * fills it with the playback state (>> / || / --) in ui_refresh(). */
    s_ui.status = ui_label(page, "", 2, UI_GRAY, &lv_font_cn_12, LV_TEXT_ALIGN_RIGHT);

    /* Header separator, matching the main-menu style. */
    lv_obj_t *sep = lv_obj_create(page);
    lv_obj_remove_style_all(sep);
    lv_obj_set_pos(sep, 0, 20);
    lv_obj_set_size(sep, LCD_H_RES, 1);
    lv_obj_set_style_bg_color(sep, lv_color_hex(UI_GRAY), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);

    if (s_ui.page_id == UI_PAGE_SETTINGS) {
        ui_build_settings(page);
        return;
    }
    if (s_ui.page_id == UI_PAGE_PLAYER) {
        ui_build_player(page);
        return;
    }
    if (s_ui.page_id == UI_PAGE_BT) {
        ui_build_bt(page);
        return;
    }
    if (s_ui.page_id == UI_PAGE_EBOOK_LIST) {
        ui_build_ebook_list(page);
        return;
    }
    if (s_ui.page_id == UI_PAGE_EBOOK_READ) {
        /* Clip the book title before it collides with the page-number
         * status label in the top-right corner. */
        lv_obj_set_width(s_ui.title, 96);
        ui_build_ebook_read(page);
        return;
    }

    /* Generic value/bar page (used by the SD CARD page). */
    s_ui.value = ui_label(page, "--", 38, UI_CYAN, &lv_font_cn_12, LV_TEXT_ALIGN_CENTER);
    s_ui.sub = ui_label(page, "--", 63, UI_GRAY, &lv_font_cn_12, LV_TEXT_ALIGN_CENTER);
    s_ui.bar = ui_bar(page, 0);
    s_ui.hint = ui_label(page, "A重扫 B返回", 106, UI_GRAY, &lv_font_cn_12, LV_TEXT_ALIGN_CENTER);
}

/* Menu uses show/hide transitions instead of LVGL swipe animations. */

static void ui_refresh_menu(void)
{
    for (int i = 0; i < UI_MENU_ROWS; i++) {
        if (i >= UI_MENU_PAGE_COUNT) {
            continue;
        }
        const int sel = (i == s_menu_sel);
        lv_label_set_text(s_ui.menu_cursor[i], sel ? ">" : " ");
        lv_obj_set_style_text_color(s_ui.menu_cursor[i], lv_color_hex(UI_CYAN), 0);
        lv_obj_set_style_text_color(s_ui.menu_text[i],
                                    lv_color_hex(sel ? UI_CYAN : UI_GRAY), 0);
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "[%d/%d]", (int)(s_menu_sel + 1), (int)UI_MENU_PAGE_COUNT);
    lv_label_set_text(s_ui.menu_status, buf);
}

static void ui_build_menu(void)
{
    lv_obj_t *mp = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(mp);
    lv_obj_set_pos(mp, 0, 0);
    lv_obj_set_size(mp, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_color(mp, lv_color_hex(UI_BG_DARK), 0);
    lv_obj_set_style_bg_opa(mp, LV_OPA_COVER, 0);
    lv_obj_clear_flag(mp, LV_OBJ_FLAG_SCROLLABLE);
    s_ui.menu_page = mp;

    /* Title bar: ASCII cat kaomoji in retro-green, left-aligned. */
    lv_obj_t *title = lv_label_create(mp);
    lv_label_set_text(title, "=^_^=");
    lv_obj_set_pos(title, 4, 2);
    lv_obj_set_style_text_font(title, &lv_font_cn_12, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_TITLE), 0);

    /* Separator line spanning the full width, clear of the title. */
    lv_obj_t *sep = lv_obj_create(mp);
    lv_obj_remove_style_all(sep);
    lv_obj_set_pos(sep, 0, 20);
    lv_obj_set_size(sep, LCD_H_RES, 1);
    lv_obj_set_style_bg_color(sep, lv_color_hex(UI_GRAY), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);

    /* Menu rows: cursor at x=8, text at x=18. */
    for (int i = 0; i < UI_MENU_ROWS; i++) {
        lv_obj_t *cur = lv_label_create(mp);
        lv_label_set_text(cur, " ");
        lv_obj_set_pos(cur, 8, s_menu_y[i]);
        lv_obj_set_style_text_font(cur, &lv_font_cn_12, 0);
        lv_obj_set_style_text_color(cur, lv_color_hex(UI_GRAY), 0);
        s_ui.menu_cursor[i] = cur;

        lv_obj_t *txt = lv_label_create(mp);
        lv_label_set_long_mode(txt, LV_LABEL_LONG_MODE_CLIP);
        lv_label_set_text(txt, i < UI_MENU_PAGE_COUNT ? s_page_names[s_menu_pages[i]] : "");
        lv_obj_set_pos(txt, 18, s_menu_y[i]);
        lv_obj_set_style_text_font(txt, &lv_font_cn_12, 0);
        lv_obj_set_style_text_color(txt, lv_color_hex(UI_GRAY), 0);
        s_ui.menu_text[i] = txt;
    }

    /* Bottom status bar: "[n/3]" left, "A:OK B:BK" right, both gray. */
    char mbuf[32];
    snprintf(mbuf, sizeof(mbuf), "[1/%d]", (int)UI_MENU_PAGE_COUNT);
    s_ui.menu_status = lv_label_create(mp);
    lv_label_set_text(s_ui.menu_status, mbuf);
    lv_obj_set_pos(s_ui.menu_status, 4, 110);
    lv_obj_set_style_text_font(s_ui.menu_status, &lv_font_cn_12, 0);
    lv_obj_set_style_text_color(s_ui.menu_status, lv_color_hex(UI_GRAY), 0);

    lv_obj_t *hint = lv_label_create(mp);
    lv_label_set_text(hint, "A:OK B:BK");
    lv_obj_set_pos(hint, 104, 110);
    lv_obj_set_style_text_font(hint, &lv_font_cn_12, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(UI_GRAY), 0);
}

static void ui_show_menu(void)
{
    s_in_menu = true;
    if (s_ui.page) {
        lv_obj_add_flag(s_ui.page, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(s_ui.menu_page, LV_OBJ_FLAG_HIDDEN);
    ui_refresh_menu();
}

static void ui_enter_page(ui_page_t page)
{
    s_in_menu = false;
    lv_obj_add_flag(s_ui.menu_page, LV_OBJ_FLAG_HIDDEN);
    if (s_ui.page) {
        lv_obj_delete(s_ui.page);
        s_ui.page = NULL;
    }
    s_ui.page_id = page;
    /* Bring the Bluetooth stack up only when the user actually opens the
     * BLUETOOTH page (it is deferred from boot). Idempotent. */
    if (page == UI_PAGE_BT) {
        bt_audio_enable();
    }
    s_ui.page = ui_make_page(0);
    s_ui.title = NULL;
    s_ui.value = NULL;
    s_ui.sub = NULL;
    s_ui.bar = NULL;
    s_ui.status = NULL;
    s_ui.hint = NULL;
    s_ui.eb_bar = NULL;
    s_ui.eb_pct = NULL;
    ui_build_page_content(s_ui.page);
    /* Force the selection highlight to repaint on the freshly built page, and
     * mark the page dirty so the first on-change refresh actually runs. */
    s_paint_set_sel = s_paint_mp3_sel = s_paint_bt_sel = -1;
    s_paint_mp3_top = -1;
    s_paint_eb_sel = -1;
    ui_mark_dirty();
    ui_refresh();
}

/* Detect engine-side state changes the UI cannot learn from its own input
 * handlers (Bluetooth stack callbacks, player engine, SD hotplug). Returns
 * true when the visible state may have changed since the last check, so
 * ui_refresh() can arm itself and recompute. Cheap: a few compares/calls. */
static bool ui_external_changed(void)
{
    bool changed = false;

    /* Bluetooth state affects both the BLUETOOTH page and the SETTINGS
     * "BT OUT" row, so watch it on every page. */
    uint32_t v   = bt_audio_device_version();
    int cnt     = bt_audio_device_count();
    bt_pair_state_t ps = bt_audio_pair_state();
    bool conn    = bt_audio_is_connected();
    bool scan    = bt_audio_is_scanning();
    if (v != s_ext_bt_ver || cnt != s_ext_bt_count || ps != s_ext_bt_pair
        || conn != s_ext_bt_conn || scan != s_ext_bt_scan) {
        s_ext_bt_ver   = v;
        s_ext_bt_count = cnt;
        s_ext_bt_pair  = ps;
        s_ext_bt_conn  = conn;
        s_ext_bt_scan  = scan;
        changed = true;
    }

    /* SD mount can change without UI input (card removed / inserted). */
    bool mnt = hw_sd_is_mounted();
    if (mnt != s_ext_sd_mounted) {
        s_ext_sd_mounted = mnt;
        player_scan_start();   /* list may have appeared / disappeared */
        changed = true;
    }

    /* Background MP3 list scan completed: repaint the player list. */
    uint32_t sv = player_scan_version();
    if (sv != s_ext_scan_ver) {
        s_ext_scan_ver = sv;
        changed = true;
    }

    /* Player state / track affects the PLAYER page. */
    if (s_ui.page_id == UI_PAGE_PLAYER) {
        player_state_t st = player_state();
        const char *nm    = player_current_name();
        if (st != s_ext_pl_state
            || strncmp(nm, s_ext_pl_name, MP3_NAME_LEN - 1) != 0) {
            s_ext_pl_state = st;
            strncpy(s_ext_pl_name, nm, MP3_NAME_LEN - 1);
            s_ext_pl_name[MP3_NAME_LEN - 1] = '\0';
            changed = true;
        }
    }

    /* Ebook: the scan list and the background page count land asynchronously. */
    if (s_ui.page_id == UI_PAGE_EBOOK_LIST) {
        uint32_t sv = ebook_scan_version();
        if (sv != s_ext_eb_scan_ver) {
            s_ext_eb_scan_ver = sv;
            changed = true;
        }
    }
    else if (s_ui.page_id == UI_PAGE_EBOOK_READ) {
        uint32_t cv = ebook_count_version();
        if (cv != s_ext_eb_cnt_ver) {
            s_ext_eb_cnt_ver = cv;
            changed = true;
        }
    }
    return changed;
}

void ui_refresh(void)
{
    ui_settings_flush();
    if (s_in_menu) {
        return;                         /* menu is event-driven */
    }

    /* An action toast is still counting down (s_action_until_ms set): keep
     * refreshing until ui_set_hint() clears it on expiry, even if nothing else
     * changed. */
    bool toast_active = (s_action_until_ms != 0);

    if (ui_external_changed()) {
        s_ui_dirty = true;             /* Bluetooth/player/SD changed via callback */
    }
    if (!s_ui_dirty && !toast_active) {
        return;                         /* idle: nothing visible changed */
    }

    /* Guards against recomputing the page before its labels are built. Leave
     * s_ui_dirty set so we retry on the next tick. */
    if (s_ui.page_id == UI_PAGE_SETTINGS) {
        if (!s_ui.set_cursor[0] || !s_ui.hint) {
            return;
        }
    }
    else if (s_ui.page_id == UI_PAGE_PLAYER) {
        if (!s_ui.pl_cursor[0] || !s_ui.hint) {
            return;
        }
    }
    else if (s_ui.page_id == UI_PAGE_BT) {
        if (!s_ui.bt_cursor[0] || !s_ui.hint) {
            return;
        }
    }
    else if (s_ui.page_id == UI_PAGE_EBOOK_LIST) {
        if (!s_ui.eb_cursor[0] || !s_ui.hint) {
            return;
        }
    }
    else if (s_ui.page_id == UI_PAGE_EBOOK_READ) {
        if (!s_ui.eb_text_label || !s_ui.hint) {
            return;
        }
    }
    else if (!s_ui.value || !s_ui.sub || !s_ui.hint) {
        return;
    }

    switch (s_ui.page_id) {
    case UI_PAGE_SD: {
        char sub[64];
        ui_label_set(s_ui.value, hw_sd_is_mounted() ? "已挂载" : "无卡");
        if (hw_sd_is_mounted()) {
            snprintf(sub, sizeof(sub), "%s  %luMB",
                     hw_sd_name(), (unsigned long)hw_sd_mb());
        }
        else {
            snprintf(sub, sizeof(sub), "GPIO22 CS  %s", short_err(hw_sd_last_err()));
        }
        ui_label_set(s_ui.sub, sub);
        ui_set_hint(hw_sd_is_mounted() ? "B返回" : "A重扫 B返回");
        ui_set_bar(hw_sd_is_mounted() ? 100 : 0);
        break;
    }
    case UI_PAGE_SETTINGS: {
        char buf[24];
        const bool sel_changed = (s_setting_sel != s_paint_set_sel);
        for (int i = 0; i < SETTING_COUNT; i++) {
            const int sel = (i == s_setting_sel);
            ui_label_set(s_ui.set_cursor[i], sel ? ">" : " ");
            if (sel_changed) {
                lv_obj_set_style_text_color(s_ui.set_cursor[i], lv_color_hex(UI_CYAN), 0);
                lv_obj_set_style_text_color(s_ui.set_text[i],
                                            lv_color_hex(sel ? UI_CYAN : UI_GRAY), 0);
                lv_obj_set_style_text_color(s_ui.set_value[i],
                                            lv_color_hex(sel ? UI_CYAN : UI_GRAY), 0);
            }
            switch (i) {
            case SETTING_VOLUME:
                snprintf(buf, sizeof(buf), "%u%%", (unsigned)hw_audio_get_volume());
                break;
            case SETTING_BTOUT:
                /* Live status: fully off, on (not linked), or linked. The
                 * persisted master switch is s_bt_on; the linked state is
                 * read live so the row reflects reality after a connect. */
                if (!s_bt_on) {
                    snprintf(buf, sizeof(buf), "关");
                }
                else if (bt_audio_is_connected()) {
                    snprintf(buf, sizeof(buf), "已连接");
                }
                else {
                    snprintf(buf, sizeof(buf), "开");
                }
                break;
            case SETTING_LOG:
                snprintf(buf, sizeof(buf), "%s", s_log_debug ? "Debug" : "Normal");
                break;
            default:
                buf[0] = '\0';
                break;
            }
            ui_label_set(s_ui.set_value[i], buf);
        }
        if (sel_changed) {
            s_paint_set_sel = s_setting_sel;
        }
        ui_set_hint("上/下选 A进入 左/右设 B返回");
        break;
    }
    case UI_PAGE_PLAYER: {
        /* The list is published by the background scan; re-fetch each pass so
         * a completed scan shows up without a page rebuild. */
        s_mp3_count = player_scan_count();
        if (s_mp3_sel >= s_mp3_count) {
            s_mp3_sel = s_mp3_count > 0 ? s_mp3_count - 1 : 0;
        }
        int top = s_mp3_sel - 1;
        if (top < 0) {
            top = 0;
        }
        if (top > s_mp3_count - MP3_LIST_ROWS) {
            top = MAX(0, s_mp3_count - MP3_LIST_ROWS);
        }
        const bool sel_changed = (s_mp3_sel != s_paint_mp3_sel)
                                 || (top != s_paint_mp3_top);
        for (int i = 0; i < MP3_LIST_ROWS; i++) {
            int idx = top + i;
            const int sel = (idx == s_mp3_sel);
            if (idx < s_mp3_count) {
                char tmp[20];
                strncpy(tmp, player_scan_name(idx), 19);
                tmp[19] = '\0';
                ui_label_set(s_ui.pl_cursor[i], sel ? ">" : " ");
                if (sel_changed) {
                    lv_obj_set_style_text_color(s_ui.pl_cursor[i], lv_color_hex(UI_CYAN), 0);
                    lv_obj_set_style_text_color(s_ui.pl_text[i],
                                                lv_color_hex(sel ? UI_CYAN : UI_GRAY), 0);
                }
                ui_label_set(s_ui.pl_text[i], tmp);
            }
            else {
                ui_label_set(s_ui.pl_cursor[i], " ");
                ui_label_set(s_ui.pl_text[i], "");
            }
        }
        if (sel_changed) {
            s_paint_mp3_sel = s_mp3_sel;
            s_paint_mp3_top = top;
        }
        player_state_t st = player_state();
        ui_label_set(s_ui.status,
                     st == PLAYER_PLAYING ? ">>" : st == PLAYER_PAUSED ? "||" : "--");
        if (st == PLAYER_IDLE) {
            if (player_scan_busy()) {
                ui_label_set(s_ui.pl_prog, "扫描中...");
            }
            else {
                ui_label_set(s_ui.pl_prog, s_mp3_count ? "空闲" : "无MP3文件");
            }
        }
        else {
            /* Now-playing line: just the track name. */
            char prog[28];
            snprintf(prog, sizeof(prog), "%s", player_current_name());
            prog[27] = '\0';
            ui_label_set(s_ui.pl_prog, prog);
        }
        if (st == PLAYER_PLAYING) {
            ui_set_hint("上/下音量 A暂停 B停止");
        }
        else if (st == PLAYER_PAUSED) {
            ui_set_hint("上/下音量 A继续 B停止");
        }
        else {
            ui_set_hint("上/下选择 A播放 B返回");
        }
        break;
    }
    case UI_PAGE_BT: {
        int count = bt_audio_device_count();
        if (s_bt_sel >= count) {
            s_bt_sel = count > 0 ? count - 1 : 0;
        }

        /* Refresh the cached device list only when bt_audio says it changed
         * (a device added, or a nameless device's name arrived). This avoids
         * re-formatting the MAC-address fallback string for every visible row
         * on every 16 ms tick. */
        uint32_t ver = bt_audio_device_version();
        if (ver != s_bt_list_ver) {
            s_bt_list_ver = ver;
            s_bt_list_cnt = count;
            for (int i = 0; i < count && i < BT_MAX_DEVICES; i++) {
                strncpy(s_bt_list_name[i], bt_audio_device_name(i),
                        BT_DEV_NAME_LEN - 1);
                s_bt_list_name[i][BT_DEV_NAME_LEN - 1] = '\0';
            }
        }

        int top = s_bt_sel - 1;
        if (top < 0) {
            top = 0;
        }
        if (top > count - BT_LIST_ROWS) {
            top = MAX(0, count - BT_LIST_ROWS);
        }
        const bool sel_changed = (s_bt_sel != s_paint_bt_sel);
        for (int i = 0; i < BT_LIST_ROWS; i++) {
            int idx = top + i;
            const int sel = (idx == s_bt_sel);
            if (idx < count) {
                const char *nm = (idx < s_bt_list_cnt && idx < BT_MAX_DEVICES)
                                 ? s_bt_list_name[idx]
                                 : bt_audio_device_name(idx);
                ui_label_set(s_ui.bt_cursor[i], sel ? ">" : " ");
                if (sel_changed) {
                    lv_obj_set_style_text_color(s_ui.bt_text[i],
                                                lv_color_hex(sel ? UI_CYAN : UI_GRAY), 0);
                }
                ui_label_set(s_ui.bt_text[i], nm);
            }
            else {
                ui_label_set(s_ui.bt_cursor[i], " ");
                ui_label_set(s_ui.bt_text[i], "");
            }
        }
        if (sel_changed) {
            s_paint_bt_sel = s_bt_sel;
        }

        if (bt_audio_is_connected()) {
            char st[28];
            snprintf(st, sizeof(st), "已连接 %s", bt_audio_peer_name());
            st[27] = '\0';
            ui_label_set(s_ui.bt_status, st);
            ui_set_hint("A断开 B返回");
        }
        else if (bt_audio_pair_state() == BT_PAIR_PAIRING) {
            /* Show the SSP passkey so the user can verify it on the sink. */
            char st[28];
            snprintf(st, sizeof(st), "配对码 %06u", (unsigned)bt_audio_passkey());
            st[27] = '\0';
            ui_label_set(s_ui.bt_status, st);
            ui_set_hint("配对中... B返回");
        }
        else if (bt_audio_pair_state() == BT_PAIR_CONNECTING) {
            char st[28];
            snprintf(st, sizeof(st), "配对中 %s", bt_audio_peer_name());
            st[27] = '\0';
            ui_label_set(s_ui.bt_status, st);
            ui_set_hint("连接中... B返回");
        }
        else if (bt_audio_pair_state() == BT_PAIR_FAIL) {
            ui_label_set(s_ui.bt_status, "配对失败");
            ui_set_hint("A重试 B返回");
        }
        else if (bt_audio_is_scanning()) {
            char st[28];
            snprintf(st, sizeof(st), "扫描中... %d", count);
            st[27] = '\0';
            ui_label_set(s_ui.bt_status, st);
            ui_set_hint("上/下选 A连接 B返回");
        }
        else {
            if (count) {
                char st[28];
                snprintf(st, sizeof(st), "%d 个设备", count);
                st[27] = '\0';
                ui_label_set(s_ui.bt_status, st);
            }
            else {
                ui_label_set(s_ui.bt_status, "无设备");
            }
            ui_set_hint(count ? "上/下选 A连接 B返回" : "A重扫 B返回");
        }
        break;
    }
    case UI_PAGE_EBOOK_LIST: {
        int count = ebook_scan_count();
        if (s_eb_sel >= count && count > 0) {
            s_eb_sel = count - 1;
        }
        int top = s_eb_sel - 1;
        if (top < 0) {
            top = 0;
        }
        if (top > count - EBOOK_LIST_ROWS) {
            top = MAX(0, count - EBOOK_LIST_ROWS);
        }
        const bool sel_changed = (s_eb_sel != s_paint_eb_sel);
        for (int i = 0; i < EBOOK_LIST_ROWS; i++) {
            int idx = top + i;
            const int sel = (idx == s_eb_sel);
            if (idx < count) {
                char tmp[64];
                strip_txt_ext(tmp, sizeof(tmp), ebook_scan_name(idx));
                ui_label_set(s_ui.eb_cursor[i], sel ? ">" : " ");
                if (sel_changed) {
                    lv_obj_set_style_text_color(s_ui.eb_cursor[i],
                                                lv_color_hex(UI_CYAN), 0);
                    lv_obj_set_style_text_color(s_ui.eb_text[i],
                                                lv_color_hex(sel ? UI_CYAN : UI_GRAY), 0);
                }
                ui_label_set(s_ui.eb_text[i], tmp);
            }
            else {
                ui_label_set(s_ui.eb_cursor[i], " ");
                ui_label_set(s_ui.eb_text[i], "");
            }
        }
        if (sel_changed) {
            s_paint_eb_sel = s_eb_sel;
        }
        if (ebook_scan_busy()) {
            ui_label_set(s_ui.eb_status, "扫描中...");
        }
        else if (count == 0) {
            ui_label_set(s_ui.eb_status, "无TXT文件");
        }
        else {
            char buf[24];
            snprintf(buf, sizeof(buf), "%d 本", count);
            ui_label_set(s_ui.eb_status, buf);
        }
        ui_set_hint("上/下选 A打开 B返回");
        break;
    }
    case UI_PAGE_EBOOK_READ: {
        ui_label_set(s_ui.title, s_eb_open_name);
        char buf[16];
        int total = ebook_page_count();
        if (total > 0) {
            snprintf(buf, sizeof(buf), "%d/%d", ebook_page(), total);
        }
        else {
            /* Total page count still computing: show just the current page. */
            snprintf(buf, sizeof(buf), "%d", ebook_page());
        }
        ui_label_set(s_ui.status, buf);
        ui_label_set(s_ui.eb_text_label, ebook_page_text());

        /* Status row: 15-cell text progress bar (left) + percentage (right). */
        int pct = ebook_percent();
        char prog[20];
        prog[0] = '[';
        int done = (pct * 15 + 50) / 100;   /* rounded to the nearest cell */
        for (int i = 0; i < 15; i++) {
            prog[1 + i] = (i < done) ? '=' : '-';
        }
        prog[16] = ']';
        prog[17] = '\0';
        ui_label_set(s_ui.eb_bar, prog);
        char pbuf[8];
        snprintf(pbuf, sizeof(pbuf), "%d%%", pct);
        ui_label_set(s_ui.eb_pct, pbuf);
        ui_set_hint("");
        break;
    }
    default:
        break;
    }
    s_ui_dirty = false;                 /* painted; wait for next change */
}

static void ui_action(void)
{
    ui_mark_dirty();                   /* an action may change visible state */
    switch (s_ui.page_id) {
    case UI_PAGE_SD:
        /* The mount blocks the UI task for tens to hundreds of ms on SDSPI,
         * so render the pending toast before it — the user sees feedback
         * immediately instead of a frozen screen. */
        set_action("重扫中...");
        lv_refr_now(NULL);
        hw_sd_try_mount();
        player_scan_start();   /* card may have been inserted / replaced */
        set_action(hw_sd_is_mounted() ? "SD已挂载" : "无SD卡");
        break;
    case UI_PAGE_PLAYER:
        if (s_mp3_count == 0) {
            set_action(player_scan_busy() ? "扫描中..." : "无MP3文件");
            break;
        }
        if (player_state() == PLAYER_PLAYING || player_state() == PLAYER_PAUSED) {
            /* Feedback first, then the toggle: the message anticipates the
             * flipped state (toggle always flips). */
            set_action(player_state() == PLAYER_PLAYING ? "已暂停" : "播放中");
            player_toggle();
        }
        else {
            set_action("播放中");
            player_play(player_scan_name(s_mp3_sel));
        }
        break;
    case UI_PAGE_BT:
        if (bt_audio_is_connected()) {
            set_action("断开中");
            bt_audio_disconnect();
        }
        else if (bt_audio_device_count() > 0) {
            set_action("连接中");
            if (!bt_audio_connect_index(s_bt_sel)) {
                set_action("连接失败");
            }
        }
        else {
            set_action("扫描中");
            bt_audio_scan_start();
        }
        break;
    case UI_PAGE_SETTINGS:
        /* The 蓝牙 item opens the Bluetooth management screen. Managing a
         * sink needs the radio, so entering it implies BT ON — set the
         * master switch and power the controller up lazily. This keeps the
         * SETTING_BTOUT row (关/开/已连接) consistent with the live state. */
        if (s_setting_sel == SETTING_BTOUT) {
            if (!s_bt_on) {
                s_bt_on = true;
                bt_audio_set_enabled(true);
                ui_settings_mark_dirty(SETTINGS_DIRTY_BT);
            }
            ui_enter_page(UI_PAGE_BT);
        }
        break;
    case UI_PAGE_EBOOK_LIST:
        if (ebook_scan_count() == 0) {
            set_action("无TXT文件");
            break;
        }
        if (!ebook_open(s_eb_sel)) {
            set_action("打开失败");
            break;
        }
        strip_txt_ext(s_eb_open_name, sizeof(s_eb_open_name),
                      ebook_scan_name(s_eb_sel));
        ui_enter_page(UI_PAGE_EBOOK_READ);
        break;
    case UI_PAGE_EBOOK_READ:
        if (ebook_at_end()) {
            set_action("最后一页");
        }
        else {
            ebook_page_flip(1);
            set_action("下一页");
        }
        break;
    default:
        break;
    }
    ui_refresh();
}

/* B (ESC) returns to the main menu via ui_show_menu(). */

static void ui_adjust(int step)
{
    ui_mark_dirty();                   /* selection / scroll changed */
    switch (s_ui.page_id) {
    case UI_PAGE_SETTINGS:
        s_setting_sel = (s_setting_sel - step + SETTING_COUNT) % SETTING_COUNT;
        set_action("选择");
        break;
    case UI_PAGE_PLAYER:
        if (player_state() == PLAYER_PLAYING ||
            player_state() == PLAYER_PAUSED) {
            /* While a track plays, up/down adjusts the output volume
             * (1% steps below 10%, else 5%). */
            ui_volume_step(step, 5);
            ui_settings_mark_dirty(SETTINGS_DIRTY_VOLUME);
            char buf[24];
            snprintf(buf, sizeof(buf), "音量 %u%%",
                     (unsigned)hw_audio_get_volume());
            set_action(buf);
        }
        else if (s_mp3_count > 0) {
            s_mp3_sel = (s_mp3_sel - step + s_mp3_count) % s_mp3_count;
            set_action("选择");
        }
        break;
    case UI_PAGE_BT: {
        int count = bt_audio_device_count();
        if (count > 0) {
            s_bt_sel = (s_bt_sel - step + count) % count;
            set_action("选择");
        }
        break;
    }
    case UI_PAGE_EBOOK_LIST: {
        int count = ebook_scan_count();
        if (count > 0) {
            s_eb_sel = (s_eb_sel - step + count) % count;
            set_action("选择");
        }
        break;
    }
    default:
        return;
    }
    ui_refresh();
}

/* Left/right changes the value of the selected settings item (or flips the
 * ebook reading page). */
static void ui_adjust_lr(int dir)
{
    ui_mark_dirty();                   /* selected setting value changed */
    if (s_ui.page_id == UI_PAGE_EBOOK_READ) {
        if (dir > 0) {
            if (ebook_at_end()) {
                set_action("最后一页");
            }
            else {
                ebook_page_flip(1);
                set_action("下一页");
            }
        }
        else {
            if (ebook_at_start()) {
                set_action("第一页");
            }
            else {
                ebook_page_flip(-1);
                set_action("上一页");
            }
        }
        ui_refresh();
        return;
    }
    if (s_ui.page_id != UI_PAGE_SETTINGS) {
        return;
    }
    switch (s_setting_sel) {
    case SETTING_VOLUME: {
        /* 1% steps below 10% for fine control, else 10%. */
        ui_volume_step(dir, 10);
        ui_settings_mark_dirty(SETTINGS_DIRTY_VOLUME);
        char buf[24];
        snprintf(buf, sizeof(buf), "VOL %u%%", (unsigned)hw_audio_get_volume());
        set_action(buf);
        break;
    }
    case SETTING_LOG:
        s_log_debug = (dir > 0);
        ui_apply_log_level();
        ui_settings_mark_dirty(SETTINGS_DIRTY_LOG);
        set_action(s_log_debug ? "Debug" : "Normal");
        break;
    case SETTING_BTOUT:
        /* Left = off, right = on. Toggling applies the routing gate and is
         * persisted to NVS on the next flush. Switching OFF also powers the
         * Bluetooth controller fully down (if it was up) to save power. */
        s_bt_on = (dir > 0);
        bt_audio_set_enabled(s_bt_on);
        if (!s_bt_on) {
            bt_audio_disable();
        }
        ui_settings_mark_dirty(SETTINGS_DIRTY_BT);
        set_action(s_bt_on ? "蓝牙开" : "蓝牙关");
        break;
    default:
        return;
    }
    ui_refresh();
}

static void ui_key_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_KEY) {
        return;
    }

    const uint32_t key = lv_event_get_key(e);

    if (s_in_menu) {
        if (key == LV_KEY_UP) {
            s_menu_sel = (s_menu_sel + UI_MENU_PAGE_COUNT - 1) % UI_MENU_PAGE_COUNT;
            ui_refresh_menu();
        }
        else if (key == LV_KEY_DOWN) {
            s_menu_sel = (s_menu_sel + 1) % UI_MENU_PAGE_COUNT;
            ui_refresh_menu();
        }
        else if (key == LV_KEY_ENTER) {
            ui_enter_page(s_menu_pages[s_menu_sel]);
        }
        /* B (ESC) is a no-op while already on the main menu. */
        return;
    }

    /* Inside a detail page. */
    if (key == LV_KEY_ESC) {
        ui_settings_flush();   /* commit any pending change before leaving */
        if (s_ui.page_id == UI_PAGE_PLAYER) {
            /* B while playing = stop but stay on the page; while idle it
             * falls through to leave back to the menu. */
            if (player_state() != PLAYER_IDLE) {
                player_stop();
                return;
            }
        }
        if (s_ui.page_id == UI_PAGE_BT) {
            /* Return to Settings, the screen this sub-page was opened from. */
            ui_enter_page(UI_PAGE_SETTINGS);
        }
        else if (s_ui.page_id == UI_PAGE_EBOOK_READ) {
            /* Back to the book list; the book stays open in the engine. */
            ui_enter_page(UI_PAGE_EBOOK_LIST);
        }
        else {
            ui_show_menu();
        }
        return;
    }
    if (key == LV_KEY_ENTER) {
        ui_action();
        return;
    }
    if (key == LV_KEY_UP) {
        ui_adjust(1);
        return;
    }
    if (key == LV_KEY_DOWN) {
        ui_adjust(-1);
        return;
    }
    if (key == LV_KEY_LEFT) {
        ui_adjust_lr(-1);
        return;
    }
    if (key == LV_KEY_RIGHT) {
        ui_adjust_lr(1);
        return;
    }
}

void ui_create(lv_group_t *group)
{
    /* Restore persisted settings (volume / log level) from NVS. */
    ui_settings_load();

    s_ui.group = group;
    s_ui.page_id = UI_PAGE_PLAYER;
    s_in_menu = true;
    s_menu_sel = 0;

    s_ui.screen = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_ui.screen);
    lv_obj_set_size(s_ui.screen, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_color(s_ui.screen, lv_color_hex(UI_BG_DARK), 0);
    lv_obj_set_style_bg_opa(s_ui.screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_ui.screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ui.screen, LV_OBJ_FLAG_CLICKABLE);
    lv_group_add_obj(group, s_ui.screen);
    lv_group_focus_obj(s_ui.screen);
    lv_obj_add_event_cb(s_ui.screen, ui_key_event_cb, LV_EVENT_KEY, NULL);
    /* Use the embedded CJK font everywhere so Chinese text renders. */
    lv_obj_set_style_text_font(s_ui.screen, UI_FONT, 0);

    ui_build_menu();
    ui_refresh_menu();
}

lv_group_t *ui_input_init(lv_display_t *display)
{
    lv_group_t *group = lv_group_create();
    assert(group);
    lv_group_set_default(group);

    lv_indev_t *indev = lv_indev_create();
    assert(indev);
    lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_display(indev, display);
    lv_indev_set_group(indev, group);
    lv_indev_set_read_cb(indev, hw_buttons_read);
    lv_indev_set_long_press_time(indev, 360);
    lv_indev_set_long_press_repeat_time(indev, 130);

    return group;
}

static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

void ui_start_tick_timer(void)
{
    const esp_timer_create_args_t tick_timer_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_timer_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, LVGL_TICK_PERIOD_MS * 1000));
}
