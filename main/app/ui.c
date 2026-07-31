/*
 * Software layer: LVGL UI and application logic.
 * See ui.h.
 */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>

#include "board_config.h"
#include "hardware/buttons.h"
#include "hardware/audio.h"
#include "hardware/bt_audio.h"
#include "hardware/sd.h"
#include "app/ui.h"
#include "player.h"

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "lvgl.h"
#include "sdkconfig.h"
#include "nvs_flash.h"
#include "nvs.h"

/* Embedded CJK bitmap font (main/fonts/lv_font_cn_10.c): ~570 KB, covers
 * ASCII + ~22000 Chinese ideographs + Japanese kana/kanji + CJK symbols.
 * Generated with lv_font_conv from SourceHanSansSC. Used as the UI default
 * so both static labels and dynamic text (SD/BT names) render Chinese. */
extern const lv_font_t lv_font_cn_10;

/* Single UI font: the CJK font includes Latin glyphs too, so there is no
 * need to mix multiple faces. */
#define UI_FONT (&lv_font_cn_10)

#define UI_ACTION_MSG_MS            850
/* Delay before persisting a changed setting to NVS, so a burst of key
 * repeats collapses into a single write. */
#define UI_SETTINGS_SAVE_DELAY_MS   800

static const char *const s_page_names[UI_PAGE_COUNT] = {
    "MP3 Player",
    "SD卡",
    "蓝牙",
    "设置",
};

/* Main menu lists only these pages. Bluetooth was moved into Settings as a
 * sub-page (opened from the 蓝牙 settings item), so it is no longer a
 * top-level tab in the main menu. */
static const ui_page_t s_menu_pages[] = {
    UI_PAGE_PLAYER, UI_PAGE_SD, UI_PAGE_SETTINGS,
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
        hw_audio_set_volume((uint8_t)v);
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
        nvs_set_i32(h, UI_NVS_VOLUME, (int32_t)hw_audio_get_volume());
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
EXT_RAM_BSS_ATTR static char s_mp3_names[64][MP3_NAME_LEN];
static int s_mp3_count;
static int s_mp3_sel;

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

    lv_group_t *group;
    ui_page_t page_id;
} ui_state_t;

static ui_state_t s_ui;
static bool s_in_menu;
static int s_menu_sel;
static uint32_t s_action_until_ms;
static char s_action[32];

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
    s_action_until_ms = lv_tick_get() + UI_ACTION_MSG_MS;
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
    if (s_action_until_ms && (int32_t)(s_action_until_ms - lv_tick_get()) > 0) {
        ui_label_set(s_ui.hint, s_action);
    }
    else {
        s_action_until_ms = 0;
        ui_label_set(s_ui.hint, normal);
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
        lv_obj_set_style_text_font(cur, &lv_font_cn_10, 0);
        lv_obj_set_style_text_color(cur, lv_color_hex(UI_GRAY), 0);
        s_ui.set_cursor[i] = cur;

        /* Fixed left-aligned label; the live value lives in its own column
         * (set_value) so items with different label lengths still line up
         * regardless of the (non-monospaced) byte width of the UTF-8 text. */
        lv_obj_t *txt = lv_label_create(page);
        lv_label_set_text(txt, labels[i]);
        lv_obj_set_pos(txt, 18, s_setting_y[i]);
        lv_obj_set_style_text_font(txt, &lv_font_cn_10, 0);
        lv_obj_set_style_text_color(txt, lv_color_hex(UI_GRAY), 0);
        s_ui.set_text[i] = txt;

        lv_obj_t *val = lv_label_create(page);
        lv_label_set_long_mode(val, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_pos(val, 95, s_setting_y[i]);
        lv_obj_set_style_text_font(val, &lv_font_cn_10, 0);
        lv_obj_set_style_text_color(val, lv_color_hex(UI_GRAY), 0);
        s_ui.set_value[i] = val;
    }

    s_ui.hint = ui_label(page, "上/下选 A进入 左/右设 B返回", 110,
                         UI_GRAY, &lv_font_cn_10, LV_TEXT_ALIGN_CENTER);
}

static void ui_build_player(lv_obj_t *page)
{
    s_mp3_sel = 0;
    s_mp3_count = 0;
    player_scan(s_mp3_names, (int)(sizeof(s_mp3_names) / sizeof(s_mp3_names[0])), &s_mp3_count);

    for (int i = 0; i < MP3_LIST_ROWS; i++) {
        lv_obj_t *cur = lv_label_create(page);
        lv_label_set_text(cur, " ");
        lv_obj_set_pos(cur, 6, s_pl_row_y[i]);
        lv_obj_set_style_text_font(cur, &lv_font_cn_10, 0);
        lv_obj_set_style_text_color(cur, lv_color_hex(UI_CYAN), 0);
        s_ui.pl_cursor[i] = cur;

        lv_obj_t *txt = lv_label_create(page);
        lv_label_set_long_mode(txt, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_pos(txt, 16, s_pl_row_y[i]);
        lv_obj_set_style_text_font(txt, &lv_font_cn_10, 0);
        lv_obj_set_style_text_color(txt, lv_color_hex(UI_GRAY), 0);
        s_ui.pl_text[i] = txt;
    }

    s_ui.pl_prog = ui_label(page, "空闲", 92, UI_GRAY, &lv_font_cn_10, LV_TEXT_ALIGN_CENTER);
    s_ui.hint = ui_label(page, "上/下选择 A播放 B返回", 110, UI_GRAY, &lv_font_cn_10, LV_TEXT_ALIGN_CENTER);
}

/* Bluetooth page: entering it kicks off a scan; the list fills live. */
static void ui_build_bt(lv_obj_t *page)
{
    s_bt_sel = 0;

    for (int i = 0; i < BT_LIST_ROWS; i++) {
        lv_obj_t *cur = lv_label_create(page);
        lv_label_set_text(cur, " ");
        lv_obj_set_pos(cur, 6, s_bt_row_y[i]);
        lv_obj_set_style_text_font(cur, &lv_font_cn_10, 0);
        lv_obj_set_style_text_color(cur, lv_color_hex(UI_CYAN), 0);
        s_ui.bt_cursor[i] = cur;

        lv_obj_t *txt = lv_label_create(page);
        lv_label_set_long_mode(txt, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_pos(txt, 16, s_bt_row_y[i]);
        lv_obj_set_style_text_font(txt, &lv_font_cn_10, 0);
        lv_obj_set_style_text_color(txt, lv_color_hex(UI_GRAY), 0);
        s_ui.bt_text[i] = txt;
    }

    s_ui.bt_status = ui_label(page, "扫描中...", 92, UI_GRAY,
                              &lv_font_cn_10, LV_TEXT_ALIGN_CENTER);
    s_ui.hint = ui_label(page, "上/下选 A连接 B返回", 110, UI_GRAY,
                         &lv_font_cn_10, LV_TEXT_ALIGN_CENTER);

    bt_audio_scan_start();
}

static void ui_build_page_content(lv_obj_t *page)
{
    char idx[10];

    s_ui.title = ui_label(page, s_page_names[s_ui.page_id], 2, UI_TITLE, &lv_font_cn_10, LV_TEXT_ALIGN_LEFT);
    /* Header number = position in the main menu. The Bluetooth sub-page
     * shows the Settings position since it is entered from there. */
    int hdr_idx = 0;
    for (int i = 0; i < UI_MENU_PAGE_COUNT; i++) {
        if (s_menu_pages[i] == s_ui.page_id) {
            hdr_idx = i;
            break;
        }
    }
    if (s_ui.page_id == UI_PAGE_BT) {
        for (int i = 0; i < UI_MENU_PAGE_COUNT; i++) {
            if (s_menu_pages[i] == UI_PAGE_SETTINGS) {
                hdr_idx = i;
                break;
            }
        }
    }
    snprintf(idx, sizeof(idx), "%02u/%02u", (unsigned)hdr_idx + 1, (unsigned)UI_MENU_PAGE_COUNT);
    s_ui.status = ui_label(page, idx, 2, UI_GRAY, &lv_font_cn_10, LV_TEXT_ALIGN_RIGHT);

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

    /* Generic value/bar page (used by the SD CARD page). */
    s_ui.value = ui_label(page, "--", 38, UI_CYAN, &lv_font_cn_10, LV_TEXT_ALIGN_CENTER);
    s_ui.sub = ui_label(page, "--", 63, UI_GRAY, &lv_font_cn_10, LV_TEXT_ALIGN_CENTER);
    s_ui.bar = ui_bar(page, 0);
    s_ui.hint = ui_label(page, "A重扫 B返回", 106, UI_GRAY, &lv_font_cn_10, LV_TEXT_ALIGN_CENTER);
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
    lv_obj_set_style_text_font(title, &lv_font_cn_10, 0);
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
        lv_obj_set_style_text_font(cur, &lv_font_cn_10, 0);
        lv_obj_set_style_text_color(cur, lv_color_hex(UI_GRAY), 0);
        s_ui.menu_cursor[i] = cur;

        lv_obj_t *txt = lv_label_create(mp);
        lv_label_set_long_mode(txt, LV_LABEL_LONG_MODE_CLIP);
        lv_label_set_text(txt, i < UI_MENU_PAGE_COUNT ? s_page_names[s_menu_pages[i]] : "");
        lv_obj_set_pos(txt, 18, s_menu_y[i]);
        lv_obj_set_style_text_font(txt, &lv_font_cn_10, 0);
        lv_obj_set_style_text_color(txt, lv_color_hex(UI_GRAY), 0);
        s_ui.menu_text[i] = txt;
    }

    /* Bottom status bar: "[n/3]" left, "A:OK B:BK" right, both gray. */
    char mbuf[32];
    snprintf(mbuf, sizeof(mbuf), "[1/%d]", (int)UI_MENU_PAGE_COUNT);
    s_ui.menu_status = lv_label_create(mp);
    lv_label_set_text(s_ui.menu_status, mbuf);
    lv_obj_set_pos(s_ui.menu_status, 4, 110);
    lv_obj_set_style_text_font(s_ui.menu_status, &lv_font_cn_10, 0);
    lv_obj_set_style_text_color(s_ui.menu_status, lv_color_hex(UI_GRAY), 0);

    lv_obj_t *hint = lv_label_create(mp);
    lv_label_set_text(hint, "A:OK B:BK");
    lv_obj_set_pos(hint, 104, 110);
    lv_obj_set_style_text_font(hint, &lv_font_cn_10, 0);
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
    ui_build_page_content(s_ui.page);
    ui_refresh();
}

void ui_refresh(void)
{
    ui_settings_flush();
    if (s_in_menu) {
        return;
    }
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
        for (int i = 0; i < SETTING_COUNT; i++) {
            const int sel = (i == s_setting_sel);
            ui_label_set(s_ui.set_cursor[i], sel ? ">" : " ");
            lv_obj_set_style_text_color(s_ui.set_cursor[i], lv_color_hex(UI_CYAN), 0);
            lv_obj_set_style_text_color(s_ui.set_text[i],
                                        lv_color_hex(sel ? UI_CYAN : UI_GRAY), 0);
            lv_obj_set_style_text_color(s_ui.set_value[i],
                                        lv_color_hex(sel ? UI_CYAN : UI_GRAY), 0);
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
        ui_set_hint("上/下选 A进入 左/右设 B返回");
        break;
    }
    case UI_PAGE_PLAYER: {
        int top = s_mp3_sel - 1;
        if (top < 0) {
            top = 0;
        }
        if (top > s_mp3_count - MP3_LIST_ROWS) {
            top = MAX(0, s_mp3_count - MP3_LIST_ROWS);
        }
        for (int i = 0; i < MP3_LIST_ROWS; i++) {
            int idx = top + i;
            const int sel = (idx == s_mp3_sel);
            if (idx < s_mp3_count) {
                char tmp[20];
                strncpy(tmp, s_mp3_names[idx], 19);
                tmp[19] = '\0';
                ui_label_set(s_ui.pl_cursor[i], sel ? ">" : " ");
                lv_obj_set_style_text_color(s_ui.pl_cursor[i], lv_color_hex(UI_CYAN), 0);
                lv_obj_set_style_text_color(s_ui.pl_text[i],
                                            lv_color_hex(sel ? UI_CYAN : UI_GRAY), 0);
                ui_label_set(s_ui.pl_text[i], tmp);
            }
            else {
                ui_label_set(s_ui.pl_cursor[i], " ");
                ui_label_set(s_ui.pl_text[i], "");
            }
        }
        player_state_t st = player_state();
        ui_label_set(s_ui.status,
                     st == PLAYER_PLAYING ? ">>" : st == PLAYER_PAUSED ? "||" : "--");
        if (st == PLAYER_IDLE) {
            ui_label_set(s_ui.pl_prog, s_mp3_count ? "空闲" : "无MP3文件");
        }
        else {
            /* Now-playing line: just the track name. */
            char prog[28];
            snprintf(prog, sizeof(prog), "%s", player_current_name());
            prog[27] = '\0';
            ui_label_set(s_ui.pl_prog, prog);
        }
        if (st == PLAYER_PLAYING || st == PLAYER_PAUSED) {
            ui_set_hint("上/下音量 A暂停 B停止");
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
        for (int i = 0; i < BT_LIST_ROWS; i++) {
            int idx = top + i;
            const int sel = (idx == s_bt_sel);
            if (idx < count) {
                const char *nm = (idx < s_bt_list_cnt && idx < BT_MAX_DEVICES)
                                 ? s_bt_list_name[idx]
                                 : bt_audio_device_name(idx);
                ui_label_set(s_ui.bt_cursor[i], sel ? ">" : " ");
                lv_obj_set_style_text_color(s_ui.bt_text[i],
                                            lv_color_hex(sel ? UI_CYAN : UI_GRAY), 0);
                ui_label_set(s_ui.bt_text[i], nm);
            }
            else {
                ui_label_set(s_ui.bt_cursor[i], " ");
                ui_label_set(s_ui.bt_text[i], "");
            }
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
    default:
        break;
    }
}

static void ui_action(void)
{
    switch (s_ui.page_id) {
    case UI_PAGE_SD:
        hw_sd_try_mount();
        set_action(hw_sd_is_mounted() ? "SD已挂载" : "无SD卡");
        break;
    case UI_PAGE_PLAYER:
        if (s_mp3_count == 0) {
            set_action("无MP3文件");
            break;
        }
        if (player_state() == PLAYER_PLAYING || player_state() == PLAYER_PAUSED) {
            player_toggle();
            set_action(player_state() == PLAYER_PAUSED ? "已暂停" : "播放中");
        }
        else {
            player_play(s_mp3_names[s_mp3_sel]);
            set_action("播放中");
        }
        break;
    case UI_PAGE_BT:
        if (bt_audio_is_connected()) {
            bt_audio_disconnect();
            set_action("断开中");
        }
        else if (bt_audio_device_count() > 0) {
            set_action(bt_audio_connect_index(s_bt_sel) ? "连接中"
                                                        : "连接失败");
        }
        else {
            bt_audio_scan_start();
            set_action("扫描中");
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
    default:
        break;
    }
    ui_refresh();
}

/* B (ESC) returns to the main menu via ui_show_menu(). */

static void ui_adjust(int step)
{
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
    default:
        return;
    }
    ui_refresh();
}

/* Left/right changes the value of the selected settings item. */
static void ui_adjust_lr(int dir)
{
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
            player_stop();
        }
        if (s_ui.page_id == UI_PAGE_BT) {
            /* Return to Settings, the screen this sub-page was opened from. */
            ui_enter_page(UI_PAGE_SETTINGS);
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
