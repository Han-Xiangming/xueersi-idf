/*
 * Settings page: volume / gain / backlight / standby / actions
 *
 * Split out of ui.c (P0 structural refactor). Behaviour and pixel layout
 * are unchanged: page-local state lives here, screen-wide state and the
 * shared helpers come from ui.c via ui_internal.h.
 */
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/param.h>
#include <sys/stat.h>

#include "audio.h"
#include "battery.h"
#include "board_config.h"
#include "buttons.h"
#include "bluetooth_audio.h"
#include "ebook.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "lcd.h"
#include "lvgl.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "player.h"
#include "sd.h"
#include "ui.h"
#include "ui_internal.h"
#include "ui_strings.h"
#include "ui_theme.h"
#include "ui_widgets.h"

/* Match the playlist row spacing (26px, starting at y=38) so the two
 * list-style pages line up visually. Seven rows: spacing compressed to
 * 24px so the last row (y=182, glyph ends ~198) clears the y=204 hint. */
/* Rows are UI_ROW_H_SETTING (26 px, matching the list pages). The flat table
 * allocates SETTING_COUNT slots, but the two-level group view only ever shows
 * up to 3 rows, so content stays above the y=218 hint. */

/* Cache-existence state shown by the "重建列表" settings item. We must NOT
 * call player_cache_exists() (a FATFS stat()) every refresh — it runs on
 * every frame while the action toast is up, and stat() on a busy SD card
 * stalls the refresh loop enough to make the settings list feel laggy when
 * scrolling. Instead we cache the result and only re-query when the playlist
 * scan version changes (i.e. a real scan/rescan/hotplug happened). */
static bool     s_cache_present;
static uint32_t s_cache_queried_ver;

/* ----- Table-driven settings --------------------------------------- */
/* Forward declarations for the per-item callbacks/value getters. */
static const char *ui_set_vol_text(void);
static const char *ui_set_gain_text(void);
static const char *ui_set_bl_text(void);
static const char *ui_set_sleep_text(void);
static const char *ui_set_rescan_text(void);
static const char *ui_set_reset_text(void);
static const char *ui_set_clear_text(void);
static const char *ui_set_autopause_text(void);
static void ui_set_vol_lr(int dir);
static void ui_set_gain_lr(int dir);
static void ui_set_bl_lr(int dir);
static void ui_set_sleep_lr(int dir);
static void ui_set_rescan_enter(void);
static void ui_set_clear_enter(void);
static void ui_set_reset_enter(void);
static void ui_set_autopause_lr(int dir);
static const char *ui_set_repeat_text(void);
static void ui_set_repeat_lr(int dir);
static const char *ui_set_bt_bitpool_text(void);
static void ui_set_bt_bitpool_lr(int dir);

/* Settings are grouped (P2): the flat list is split into Audio / Display /
 * System. The launcher-stle two-level view (group list -> group items) reuses
 * the same 8 row widgets; only what each row shows changes per level. */
typedef enum {
    SET_GRP_AUDIO = 0,
    SET_GRP_DISPLAY,
    SET_GRP_SYSTEM,
    SET_GRP_COUNT,
} settings_group_t;

static const char *s_group_names[SET_GRP_COUNT] = {
    [SET_GRP_AUDIO]   = "音频",
    [SET_GRP_DISPLAY] = "显示",
    [SET_GRP_SYSTEM]  = "系统",
};

/* Group -> member items (indices into s_settings_table). Column width 5;
 * s_group_cnt says how many of each row are live. */
static const setting_item_t s_group_items[SET_GRP_COUNT][5] = {
    [SET_GRP_AUDIO]   = {SETTING_VOLUME, SETTING_MASTER_GAIN, SETTING_AUTOPAUSE_ROUTE, SETTING_REPEAT, SETTING_BT_BITPOOL},
    [SET_GRP_DISPLAY] = {SETTING_BACKLIGHT, SETTING_STANDBY},
    [SET_GRP_SYSTEM]  = {SETTING_RESCAN, SETTING_RESET, SETTING_CLEAR_PROG},
};
static const int s_group_cnt[SET_GRP_COUNT] = {
    [SET_GRP_AUDIO] = 5, [SET_GRP_DISPLAY] = 2, [SET_GRP_SYSTEM] = 3,
};

/* A single settings row descriptor. Adding a setting = appending one row to
 * s_settings_table (and the matching SETTING_* enum). No switch/loop edits. */
typedef struct {
    const char *label;                  /* left-side label */
    const char *(*value_fn)(void);      /* right-side live value text */
    void (*on_lr)(int dir);             /* LEFT/RIGHT adjust (dir: -1/+1) */
    void (*on_enter)(void);             /* A press (NULL = not actionable) */
    settings_group_t group;             /* which group page it lives under */
} setting_entry_t;

static const setting_entry_t s_settings_table[SETTING_COUNT] = {
    [SETTING_NONE]        = { NULL, NULL, NULL, NULL, SET_GRP_AUDIO },   /* sentinel, never shown */
    [SETTING_VOLUME]      = {UI_STR_SET_VOL,    ui_set_vol_text,   ui_set_vol_lr,   NULL,              SET_GRP_AUDIO},
    [SETTING_MASTER_GAIN] = {UI_STR_SET_GAIN,  ui_set_gain_text,  ui_set_gain_lr,  NULL,              SET_GRP_AUDIO},
    [SETTING_BACKLIGHT]   = {UI_STR_SET_BACKLIGHT,    ui_set_bl_text,    ui_set_bl_lr,    NULL,              SET_GRP_DISPLAY},
    [SETTING_STANDBY]     = {UI_STR_SET_STANDBY,    ui_set_sleep_text, ui_set_sleep_lr, NULL,              SET_GRP_DISPLAY},
    [SETTING_RESCAN]      = {UI_STR_SET_RESCAN, ui_set_rescan_text, NULL,       ui_set_rescan_enter, SET_GRP_SYSTEM},
    [SETTING_RESET]       = {UI_STR_SET_RESET, ui_set_reset_text, NULL,             ui_set_reset_enter, SET_GRP_SYSTEM},
    [SETTING_CLEAR_PROG]  = {UI_STR_SET_CLEAR_PROG, ui_set_clear_text, NULL,        ui_set_clear_enter, SET_GRP_SYSTEM},
    [SETTING_AUTOPAUSE_ROUTE] = {UI_STR_SET_AUTOPAUSE, ui_set_autopause_text, ui_set_autopause_lr, NULL, SET_GRP_AUDIO},
    [SETTING_REPEAT]          = {UI_STR_SET_REPEAT,    ui_set_repeat_text,    ui_set_repeat_lr,    NULL, SET_GRP_AUDIO},
    [SETTING_BT_BITPOOL]       = {UI_STR_SET_BT_BITPOOL, ui_set_bt_bitpool_text, ui_set_bt_bitpool_lr, NULL, SET_GRP_AUDIO},
};

/* Backlight brightness (0..100 %), driven via PWM on PIN_NUM_LCD_BL.
 * Persisted to NVS; restored at boot. */
static uint8_t s_backlight = 60;

/* Auto screen-off: idle timeout in seconds. 0 = never (disable standby).
 * Selectable on the settings page via an index into s_standby_opts. */
typedef enum {
    STANDBY_OPT_NEVER = 0,
    STANDBY_OPT_15S,
    STANDBY_OPT_30S,
    STANDBY_OPT_60S,
    STANDBY_OPT_2MIN,
    STANDBY_OPT_5MIN,
    STANDBY_OPT_COUNT,
} standby_opt_t;

/* Idle timeout (seconds) for each option index. Index 0 is "never". */
static const uint16_t s_standby_opts[STANDBY_OPT_COUNT] = {
    0, 15, 30, 60, 120, 300,
};
static standby_opt_t s_standby_opt = STANDBY_OPT_30S;  /* default 30 s */

/* Auto-pause on output-route change (speaker <-> bluetooth). Off by default:
 * a route switch keeps playing on the new output. Persisted to NVS; the
 * runtime flag lives in the player (player_set_autopause_on_route_change). */
static bool s_autopause_route = false;

/* Bluetooth audio quality (SBC bitpool preset). Three steps trade link
 * robustness for fidelity: 稳定(33) / 均衡(45) / 高质(53). Persisted to NVS
 * and pushed to the driver, which applies it on the next Bluetooth connect. */
typedef enum {
    BT_BITPOOL_STABLE = 0,   /* 33 ~229 kbps, most robust on weak sinks */
    BT_BITPOOL_BALANCED,     /* 45 ~295 kbps */
    BT_BITPOOL_QUALITY,      /* 53 ~328 kbps, best quality for 44.1 kHz */
    BT_BITPOOL_COUNT,
} bt_bitpool_preset_t;
static const uint8_t s_bt_bitpool_vals[BT_BITPOOL_COUNT] = {33, 45, 53};
static const char *s_bt_bitpool_names[BT_BITPOOL_COUNT] = {"稳定", "均衡", "高质"};
static bt_bitpool_preset_t s_bt_bitpool_preset = BT_BITPOOL_STABLE;

/* Selected row, and the paint guard that remembers what is currently drawn
 * (-1 forces a repaint right after the page is rebuilt). */
static int s_setting_sel = 0;
static int s_paint_set_sel = -1;

/* Two-level view: the group list, or the items inside the selected group. The
 * same 8 row widgets are reused; only their contents differ per level. */
typedef enum { SET_VIEW_GROUPS = 0, SET_VIEW_ITEMS } settings_view_t;
static settings_view_t s_set_view = SET_VIEW_GROUPS;
static int s_set_group = 0;   /* which group the ITEMS view is showing */

/* Settings persistence: volume, gain, backlight, standby and the route
 * auto-pause flag survive reboot via NVS. NVS is initialised in app_main()
 * before ui_create(), so these helpers can open the handle at any time. */
#define UI_NVS_NS      "ui_cfg"
#define UI_NVS_VOLUME  "volume"
#define UI_NVS_VOLBT   "vol_bt"
#define UI_NVS_GAIN    "gain_db"
#define UI_NVS_BACKL   "backlight"
#define UI_NVS_STBY    "standby_s"
#define UI_NVS_AUTOPAUSE "autopause_route"
#define UI_NVS_REPEAT    "repeat"
#define UI_NVS_BT_BITPOOL "bt_bitpool"

void ui_settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open(UI_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    int32_t v = -1;
    if (nvs_get_i32(h, UI_NVS_VOLUME, &v) == ESP_OK && v >= 0 && v <= 100) {
        hw_audio_set_speaker_volume((uint8_t)v);
    } else {
        hw_audio_set_speaker_volume(30);   /* default speaker volume 30% */
    }
    v = -1;
    if (nvs_get_i32(h, UI_NVS_VOLBT, &v) == ESP_OK && v >= 0 && v <= 100) {
        hw_audio_set_bt_volume((uint8_t)v);
    } else {
        hw_audio_set_bt_volume(30);        /* default BT volume 30% */
    }
    v = -1;
    if (nvs_get_i32(h, UI_NVS_GAIN, &v) == ESP_OK &&
        v >= -120 && v <= 120) {
        hw_audio_set_master_gain_db((float)v / 10.0f);
    } else {
        hw_audio_set_master_gain_db(0.0f); /* default master gain 0 dB */
    }
    int32_t bl = -1;
    if (nvs_get_i32(h, UI_NVS_BACKL, &bl) == ESP_OK && bl >= 0 && bl <= 100) {
        s_backlight = (uint8_t)bl;
        hw_lcd_set_backlight(s_backlight);
    } else {
        hw_lcd_set_backlight(s_backlight);
    }
    int32_t stby = -1;
    if (nvs_get_i32(h, UI_NVS_STBY, &stby) == ESP_OK &&
        stby >= 0 && stby < STANDBY_OPT_COUNT) {
        s_standby_opt = (standby_opt_t)stby;
    }
    hw_lcd_set_standby_timeout((uint32_t)s_standby_opts[s_standby_opt] * 1000);
    int32_t autopause = -1;
    if (nvs_get_i32(h, UI_NVS_AUTOPAUSE, &autopause) == ESP_OK) {
        s_autopause_route = (autopause != 0);
    }
    player_set_autopause_on_route_change(s_autopause_route);
    int32_t repeat = -1;
    if (nvs_get_i32(h, UI_NVS_REPEAT, &repeat) == ESP_OK &&
        repeat >= PLAYER_REPEAT_ALL && repeat <= PLAYER_REPEAT_RANDOM) {
        player_set_repeat_mode((player_repeat_t)repeat);
    }
    int32_t bp = -1;
    if (nvs_get_i32(h, UI_NVS_BT_BITPOOL, &bp) == ESP_OK &&
        bp >= BT_BITPOOL_STABLE && bp < BT_BITPOOL_COUNT) {
        s_bt_bitpool_preset = (bt_bitpool_preset_t)bp;
    }
    /* Apply the chosen SBC bitpool cap to the A2DP SEP (takes effect on the
     * next Bluetooth connection). Safe no-op when BT is disabled in the build. */
    bluetooth_audio_set_bitpool(s_bt_bitpool_vals[s_bt_bitpool_preset]);
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

static void ui_settings_save_gain(void)
{
    nvs_handle_t h;
    if (nvs_open(UI_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        /* dB x10, rounded (the UI always sets whole dB, so this is exact). */
        float db = hw_audio_get_master_gain_db();
        nvs_set_i32(h, UI_NVS_GAIN,
                    (int32_t)(db * 10.0f + (db >= 0.0f ? 0.5f : -0.5f)));
        nvs_commit(h);
        nvs_close(h);
    }
}

static void ui_settings_save_backlight(void)
{
    nvs_handle_t h;
    if (nvs_open(UI_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, UI_NVS_BACKL, (int32_t)s_backlight);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void ui_settings_save_standby(void)
{
    nvs_handle_t h;
    if (nvs_open(UI_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, UI_NVS_STBY, (int32_t)s_standby_opt);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void ui_settings_save_autopause(void)
{
    nvs_handle_t h;
    if (nvs_open(UI_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, UI_NVS_AUTOPAUSE, s_autopause_route ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void ui_settings_save_repeat(void)
{
    nvs_handle_t h;
    if (nvs_open(UI_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, UI_NVS_REPEAT, (int32_t)player_repeat_mode());
        nvs_commit(h);
        nvs_close(h);
    }
}

static void ui_settings_save_bt_bitpool(void)
{
    nvs_handle_t h;
    if (nvs_open(UI_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, UI_NVS_BT_BITPOOL, (int32_t)s_bt_bitpool_preset);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* Debounced persistence: a setting change only marks a dirty bit and arms a
 * timer; ui_settings_flush() (called every refresh) commits once the user
 * stops tweaking, folding long-press repeats into a single NVS write. */
static uint32_t s_save_pending;
static uint32_t s_save_at_ms;

/* Keep the settings page's backlight copy in sync when the brightness is
 * stepped from elsewhere (the ebook reader's ↑↓): without this the row would
 * show a stale value and step from the wrong base. Marks the value dirty so
 * the debounced flush persists it. */
void ui_settings_set_backlight_percent(uint8_t pct)
{
    s_backlight = pct;
    ui_settings_mark_dirty(SETTINGS_DIRTY_BACKL);
}

void ui_settings_mark_dirty(uint32_t which)
{
    s_save_pending |= which;
    s_save_at_ms = lv_tick_get() + UI_SETTINGS_SAVE_DELAY_MS;
}

void ui_settings_flush(void)
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
    if (s_save_pending & SETTINGS_DIRTY_GAIN) {
        ui_settings_save_gain();
    }
    if (s_save_pending & SETTINGS_DIRTY_BACKL) {
        ui_settings_save_backlight();
    }
    if (s_save_pending & SETTINGS_DIRTY_STBY) {
        ui_settings_save_standby();
    }
    if (s_save_pending & SETTINGS_DIRTY_AUTOPAUSE) {
        ui_settings_save_autopause();
    }
    if (s_save_pending & SETTINGS_DIRTY_REPEAT) {
        ui_settings_save_repeat();
    }
    if (s_save_pending & SETTINGS_DIRTY_BT_BITPOOL) {
        ui_settings_save_bt_bitpool();
    }
    s_save_pending = 0;
}
void ui_refresh_settings(void)
{
    /* Re-query cache existence only when the playlist scan version changes, not
     * every frame — avoids hammering FATFS stat() while a toast is showing. */
    const uint32_t scan_ver = player_scan_version();
    if (scan_ver != s_cache_queried_ver) {
        s_cache_present = player_cache_exists();
        s_cache_queried_ver = scan_ver;
    }

    /* Title bar reflects the two-level navigation: "设置" on the group list,
     * "设置 > 子菜单" once a group's items are open. */
    if (s_set_view == SET_VIEW_ITEMS) {
        static char s_title_buf[48];
        snprintf(s_title_buf, sizeof(s_title_buf), "设置 > %s",
                 s_group_names[s_set_group]);
        ui_theme_text_set(s_ui.title, s_title_buf);
    } else {
        ui_theme_text_set(s_ui.title, "设置");
    }

    /* Both views reuse the same 8 row widgets; only the row contents differ. */
    if (s_set_view == SET_VIEW_GROUPS) {
        const int n = SET_GRP_COUNT;
        for (int i = 0; i < SETTING_COUNT; i++) {
            if (i < n) {
                const int sel = (i == s_setting_sel);
                /* Focus fill + accent text, painted every frame so a rebuild
                 * can't leave a stale highlight on the wrong row. */
                if (sel) {
                    lv_obj_set_style_bg_color(s_ui.set_row[i],
                                              ui_theme_color(UI_COLOR_PANEL), 0);
                    lv_obj_set_style_bg_opa(s_ui.set_row[i], LV_OPA_COVER, 0);
                }
                else {
                    lv_obj_set_style_bg_opa(s_ui.set_row[i], LV_OPA_TRANSP, 0);
                }
                ui_theme_text_set(s_ui.set_cursor[i], sel ? ">" : " ");
                ui_theme_set_color(s_ui.set_cursor[i], UI_COLOR_ACCENT);
                ui_theme_set_color(s_ui.set_text[i],
                                   sel ? UI_COLOR_ACCENT : UI_COLOR_TEXT_DIM);
                ui_theme_text_set(s_ui.set_text[i], s_group_names[i]);
                ui_theme_set_color(s_ui.set_value[i], UI_COLOR_TEXT_DIM);
                ui_theme_text_set(s_ui.set_value[i], "");
            }
            else {
                ui_theme_text_set(s_ui.set_cursor[i], " ");
                ui_theme_text_set(s_ui.set_text[i], "");
                ui_theme_text_set(s_ui.set_value[i], "");
                lv_obj_set_style_bg_opa(s_ui.set_row[i], LV_OPA_TRANSP, 0);
            }
        }
        s_paint_set_sel = s_setting_sel;
        ui_set_hint("A:进入  B:返回");
        return;
    }

    /* ITEMS view: show the selected group's members. */
    const int n = s_group_cnt[s_set_group];
    const setting_item_t *items = s_group_items[s_set_group];
    for (int i = 0; i < SETTING_COUNT; i++) {
        if (i < n && items[i] != SETTING_NONE) {
            const int sel = (i == s_setting_sel);
            const setting_entry_t *e = &s_settings_table[items[i]];
            if (sel) {
                lv_obj_set_style_bg_color(s_ui.set_row[i],
                                          ui_theme_color(UI_COLOR_PANEL), 0);
                lv_obj_set_style_bg_opa(s_ui.set_row[i], LV_OPA_COVER, 0);
            }
            else {
                lv_obj_set_style_bg_opa(s_ui.set_row[i], LV_OPA_TRANSP, 0);
            }
            ui_theme_text_set(s_ui.set_cursor[i], sel ? ">" : " ");
            ui_theme_set_color(s_ui.set_cursor[i], UI_COLOR_ACCENT);
            ui_theme_set_color(s_ui.set_text[i],
                               sel ? UI_COLOR_ACCENT : UI_COLOR_TEXT_DIM);
            ui_theme_set_color(s_ui.set_value[i],
                               sel ? UI_COLOR_ACCENT : UI_COLOR_TEXT_DIM);
            ui_theme_text_set(s_ui.set_text[i], e->label);
            ui_theme_text_set(s_ui.set_value[i], e->value_fn ? e->value_fn() : "");
        }
        else {
            ui_theme_text_set(s_ui.set_cursor[i], " ");
            ui_theme_text_set(s_ui.set_text[i], "");
            ui_theme_text_set(s_ui.set_value[i], "");
            lv_obj_set_style_bg_opa(s_ui.set_row[i], LV_OPA_TRANSP, 0);
        }
    }
    s_paint_set_sel = s_setting_sel;
    ui_set_hint("A:确定  ←→调  B:返回");
}

void ui_settings_action(void)
{
    if (s_set_view == SET_VIEW_GROUPS) {
        /* Drill into the selected group. */
        s_set_group = s_setting_sel;
        s_set_view = SET_VIEW_ITEMS;
        s_setting_sel = 0;
        s_paint_set_sel = -1;
        ui_mark_dirty();
        ui_refresh();
        return;
    }
    /* ITEMS view: dispatch to the selected item's on_enter (action items like
     * 重建列表 / 恢复出厂). Inert / empty (SETTING_NONE) slots do nothing. */
    const setting_item_t item = s_group_items[s_set_group][s_setting_sel];
    if (item == SETTING_NONE) {
        return;
    }
    const setting_entry_t *e = &s_settings_table[item];
    if (e->on_enter) {
        e->on_enter();
    }
}

void ui_settings_adjust(int step)
{
    const int n = (s_set_view == SET_VIEW_GROUPS)
                    ? SET_GRP_COUNT
                    : s_group_cnt[s_set_group];
    s_setting_sel = (s_setting_sel - step + n) % n;
}

void ui_settings_lr(int dir)
{
    if (s_set_view != SET_VIEW_ITEMS) {
        return;
    }
    const setting_item_t item = s_group_items[s_set_group][s_setting_sel];
    if (item == SETTING_NONE) {
        return;
    }
    const setting_entry_t *e = &s_settings_table[item];
    if (e->on_lr) {
        e->on_lr(dir);          /* callback owns dirty-marking + set_action */
        ui_refresh();
    }
}

/* B on the items view steps back to the group list; on the group list it lets
 * the caller pop the navigation stack. Returns true when handled internally. */
bool ui_settings_esc(void)
{
    if (s_set_view == SET_VIEW_ITEMS) {
        s_set_view = SET_VIEW_GROUPS;
        s_setting_sel = s_set_group;
        s_paint_set_sel = -1;
        ui_mark_dirty();
        ui_refresh();
        return true;
    }
    return false;
}

void ui_settings_reset_paint(void)
{
    s_paint_set_sel = -1;
    s_set_view = SET_VIEW_GROUPS;
    s_set_group = 0;
    s_setting_sel = 0;
}


/* Each entry's live value text. Returned strings live in function-local
 * static buffers — safe because the UI is driven by a single task. */

static const char *ui_set_vol_text(void)
{
    static char buf[24];
    snprintf(buf, sizeof(buf), "%u%%", (unsigned)hw_audio_get_volume());
    return buf;
}

static const char *ui_set_gain_text(void)
{
    static char buf[24];
    /* Signed dB: "+6dB" / "0dB" / "-3dB". */
    int db = (int)hw_audio_get_master_gain_db();
    snprintf(buf, sizeof(buf), db > 0 ? "+%ddB" : "%ddB", db);
    return buf;
}

static const char *ui_set_bl_text(void)
{
    static char buf[24];
    snprintf(buf, sizeof(buf), "%u%%", (unsigned)s_backlight);
    return buf;
}

static const char *ui_set_sleep_text(void)
{
    static char buf[24];
    if (s_standby_opt == STANDBY_OPT_NEVER) {
        snprintf(buf, sizeof(buf), UI_STR_NEVER);
    } else {
        snprintf(buf, sizeof(buf), "%u秒",
                 (unsigned)s_standby_opts[s_standby_opt]);
    }
    return buf;
}

static const char *ui_set_rescan_text(void)
{
    static char buf[24];
    /* Action item: show whether a cache is currently present so the user
     * knows if the list is being served from it. Pressing A forces a fresh
     * full-card scan that rewrites the cache. s_cache_present is refreshed
     * only when the scan version changes (in the refresh loop), never
     * per-frame. */
    snprintf(buf, sizeof(buf), s_cache_present ? UI_STR_CACHE_YES : UI_STR_CACHE_NO);
    return buf;
}

static const char *ui_set_reset_text(void)
{
    return UI_STR_RESTORE;
}

/* Left/right adjust callbacks. Each owns its dirty-mark + set_action. */

static void ui_set_vol_lr(int dir)
{
    /* Step + persist + toast all handled by the shared volume entry point. */
    ui_volume_change(dir);
}

static void ui_set_gain_lr(int dir)
{
    /* Whole dB steps over the preamp range ±12 dB. */
    int db = (int)hw_audio_get_master_gain_db() + dir;
    db = MAX(-12, MIN(db, 12));
    hw_audio_set_master_gain_db((float)db);
    ui_settings_mark_dirty(SETTINGS_DIRTY_GAIN);
}

static void ui_set_bl_lr(int dir)
{
    /* Step + persist + toast all handled by the shared backlight entry point
     * (same one the ebook reader's ↑↓ uses, so both stay in lockstep). */
    ui_backlight_change(dir);
}

static void ui_set_sleep_lr(int dir)
{
    int opt = (int)s_standby_opt + dir;
    opt = MAX(0, MIN(opt, (int)STANDBY_OPT_COUNT - 1));
    s_standby_opt = (standby_opt_t)opt;
    hw_lcd_set_standby_timeout((uint32_t)s_standby_opts[s_standby_opt] * 1000);
    ui_settings_mark_dirty(SETTINGS_DIRTY_STBY);
}

/* A-press (enter) callbacks. */

static void ui_set_rescan_enter(void)
{
    /* Drop the on-card playlist cache and rebuild it from a fresh, full-card
     * scan. The scan is asynchronous; the player falls back to a real scan
     * whenever the cache is absent, so this is safe even while playing. */
    player_rescan();
    set_action(UI_STR_RESCANNING);
}

static void ui_set_reset_enter(void)
{
    /* Restore NVS to factory defaults: wipe the whole NVS partition and
     * reboot. Boot will re-create every setting at its default. */
    set_action(UI_STR_RESETTING);
    ui_refresh();
    nvs_flash_erase();
    esp_restart();
}

static const char *ui_set_clear_text(void)
{
    return UI_STR_CLEAR_A;
}

static void ui_set_clear_enter(void)
{
    /* Wipe all saved reading positions (v2 + legacy v1) so books that were
     * resuming to the wrong place start fresh at page 1. */
    ebook_progress_clear_all();
    set_action(UI_STR_CLEARED);
    ui_refresh();
}

/* ----- 路由变更暂停（音频组） ----- */

static const char *ui_set_autopause_text(void)
{
    static char buf[24];
    snprintf(buf, sizeof(buf), "%s", s_autopause_route ? "开" : "关");
    return buf;
}

static void ui_set_autopause_lr(int dir)
{
    /* Left = off, right = on. Pushed to the player at once so a route change
     * in the same session already honours it; persisted on the next flush. */
    s_autopause_route = (dir > 0);
    player_set_autopause_on_route_change(s_autopause_route);
    ui_settings_mark_dirty(SETTINGS_DIRTY_AUTOPAUSE);
    set_action(s_autopause_route ? "路由变更暂停开" : "路由变更暂停关");
}

/* ----- 循环模式（音频组） ----- */

static const char *ui_set_repeat_text(void)
{
    /* Same short labels the player/panel use: 列表 / 单曲 / 随机. */
    switch (player_repeat_mode()) {
    case PLAYER_REPEAT_ONE:    return UI_STR_REPEAT_ONE;
    case PLAYER_REPEAT_RANDOM: return UI_STR_REPEAT_RANDOM;
    default:                   return UI_STR_REPEAT_LIST;
    }
}

static void ui_set_repeat_lr(int dir)
{
    /* Step the mode by dir (wrapping); LEFT/RIGHT both cycle, just in opposite
     * directions. Use signed int for the arithmetic + bounds check: the enum is
     * unsigned-backed (all enumerators are non-negative), so a dir of -1 would
     * otherwise wrap to a huge value and miss the <ALL branch. Pushed to the
     * player at once and persisted on the next flush. */
    int m = (int)player_repeat_mode() + dir;
    if (m < PLAYER_REPEAT_ALL) {
        m = PLAYER_REPEAT_RANDOM;
    } else if (m > PLAYER_REPEAT_RANDOM) {
        m = PLAYER_REPEAT_ALL;
    }
    player_set_repeat_mode((player_repeat_t)m);
    ui_settings_mark_dirty(SETTINGS_DIRTY_REPEAT);
    set_action(m == PLAYER_REPEAT_ONE
                   ? UI_STR_LOOP_ONE
                   : m == PLAYER_REPEAT_RANDOM ? UI_STR_LOOP_RANDOM
                                               : UI_STR_LOOP_LIST);
}

/* ----- 蓝牙音质 / SBC bitpool 预设（音频组） ----- */

static const char *ui_set_bt_bitpool_text(void)
{
    /* 稳定 / 均衡 / 高质 — the chosen preset's short name. */
    return s_bt_bitpool_names[s_bt_bitpool_preset];
}

static void ui_set_bt_bitpool_lr(int dir)
{
    /* LEFT = more robust (lower bitpool), RIGHT = higher quality. Cycle
     * through the presets, wrapping at both ends, so every step is always
     * reachable. Pushed to the driver at once (applies on next BT connect)
     * and persisted on flush. */
    int p = (int)s_bt_bitpool_preset + dir;
    if (p < 0) {
        p = BT_BITPOOL_COUNT - 1;
    } else if (p >= BT_BITPOOL_COUNT) {
        p = 0;
    }
    s_bt_bitpool_preset = (bt_bitpool_preset_t)p;
    bluetooth_audio_set_bitpool(s_bt_bitpool_vals[s_bt_bitpool_preset]);
    ui_settings_mark_dirty(SETTINGS_DIRTY_BT_BITPOOL);
    set_action(s_bt_bitpool_names[s_bt_bitpool_preset]);
}

void ui_build_settings(lv_obj_t *page)
{
    s_setting_sel = 0;
    s_set_view = SET_VIEW_GROUPS;
    s_set_group = 0;
    /* Force a fresh cache-existence query on the next refresh (the cached
     * value may be stale from a previous visit). */
    s_cache_queried_ver = 0;
    /* Force a battery refresh (bypasses the freeze-while-playing guard) so the
     * gauge shows a live reading even if audio is playing. */
    hw_battery_sample();

    for (int i = 0; i < SETTING_COUNT; i++) {
        const int y = UI_SETTINGS_FIRST_Y + i * UI_ROW_H_SETTING;

        /* Row container carrying the focus fill (same treatment as the
         * ui_list_t rows on the other pages). */
        lv_obj_t *row = lv_obj_create(page);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, y);
        lv_obj_set_size(row, UI_SCREEN_W, UI_ROW_H_SETTING);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        /* The 16px font's line_height (30) is taller than this 20px row, and LVGL
         * clips children to the parent by default — which would shave the bottom
         * of every label. Let the text paint past the row box (it still lands
         * well inside the screen and never overlaps the next row). */
        lv_obj_add_flag(row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        s_ui.set_row[i] = row;

        lv_obj_t *cur = lv_label_create(row);
        lv_label_set_text(cur, " ");
        lv_obj_set_pos(cur, 8, 0);
        lv_obj_set_style_text_font(cur, UI_FONT, 0);
        lv_obj_set_style_text_color(cur, lv_color_hex(UI_COLOR_ACCENT), 0);
        s_ui.set_cursor[i] = cur;

        /* Fixed left-aligned label; the live value lives in its own column
         * (set_value) so items with different label lengths still line up
         * regardless of the (non-monospaced) byte width of the UTF-8 text. */
        lv_obj_t *txt = lv_label_create(row);
        lv_label_set_text(txt, s_settings_table[i].label);
        lv_obj_set_pos(txt, 18, 0);
        lv_obj_set_style_text_font(txt, UI_FONT, 0);
        lv_obj_set_style_text_color(txt, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
        s_ui.set_text[i] = txt;

        lv_obj_t *val = lv_label_create(row);
        lv_label_set_long_mode(val, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_size(val, 80, LV_SIZE_CONTENT);
        lv_obj_set_pos(val, 232, 0);
        lv_obj_set_style_text_font(val, UI_FONT, 0);
        lv_obj_set_style_text_color(val, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
        lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);
        s_ui.set_value[i] = val;
    }

    s_ui.hint = ui_theme_label(page, UI_STR_HINT_NAV_LR, UI_LEGEND_Y,
                         UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_CENTER);
}
