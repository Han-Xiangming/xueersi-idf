/*
 * Settings page: volume / gain / backlight / BT / standby / actions
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
#include "bluetooth_audio.h"
#include "board_config.h"
#include "buttons.h"
#include "ebook.h"
#include "esp_attr.h"
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
static const char *ui_set_bt_text(void);
static const char *ui_set_sleep_text(void);
static const char *ui_set_rescan_text(void);
static const char *ui_set_reset_text(void);
static const char *ui_set_clear_text(void);
static void ui_set_vol_lr(int dir);
static void ui_set_gain_lr(int dir);
static void ui_set_bl_lr(int dir);
static void ui_set_bt_lr(int dir);
static void ui_set_sleep_lr(int dir);
static void ui_set_rescan_enter(void);
static void ui_set_clear_enter(void);
static void ui_set_reset_enter(void);
static void ui_set_bt_enter(void);

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

/* Group -> member items (indices into s_settings_table). Column width 4;
 * s_group_cnt says how many of each row are live. */
static const setting_item_t s_group_items[SET_GRP_COUNT][4] = {
    [SET_GRP_AUDIO]   = {SETTING_VOLUME, SETTING_MASTER_GAIN, SETTING_BTOUT},
    [SET_GRP_DISPLAY] = {SETTING_BACKLIGHT, SETTING_STANDBY},
    [SET_GRP_SYSTEM]  = {SETTING_RESCAN, SETTING_RESET, SETTING_CLEAR_PROG},
};
static const int s_group_cnt[SET_GRP_COUNT] = {
    [SET_GRP_AUDIO] = 3, [SET_GRP_DISPLAY] = 2, [SET_GRP_SYSTEM] = 3,
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
    [SETTING_VOLUME]      = {UI_STR_SET_VOL,    ui_set_vol_text,   ui_set_vol_lr,   NULL,              SET_GRP_AUDIO},
    [SETTING_MASTER_GAIN] = {UI_STR_SET_GAIN,  ui_set_gain_text,  ui_set_gain_lr,  NULL,              SET_GRP_AUDIO},
    [SETTING_BACKLIGHT]   = {UI_STR_SET_BACKLIGHT,    ui_set_bl_text,    ui_set_bl_lr,    NULL,              SET_GRP_DISPLAY},
    [SETTING_BTOUT]       = {"蓝牙",    ui_set_bt_text,    ui_set_bt_lr,    ui_set_bt_enter,  SET_GRP_AUDIO},
    [SETTING_STANDBY]     = {UI_STR_SET_STANDBY,    ui_set_sleep_text, ui_set_sleep_lr, NULL,              SET_GRP_DISPLAY},
    [SETTING_RESCAN]      = {UI_STR_SET_RESCAN, ui_set_rescan_text, NULL,       ui_set_rescan_enter, SET_GRP_SYSTEM},
    [SETTING_RESET]       = {UI_STR_SET_RESET, ui_set_reset_text, NULL,             ui_set_reset_enter, SET_GRP_SYSTEM},
    [SETTING_CLEAR_PROG]  = {UI_STR_SET_CLEAR_PROG, ui_set_clear_text, NULL,        ui_set_clear_enter, SET_GRP_SYSTEM},
};

/* Backlight brightness (0..100 %), driven via PWM on PIN_NUM_LCD_BL.
 * Persisted to NVS; restored at boot. */
static uint8_t s_backlight = 60;

/* Bluetooth output master switch (settings page ON/OFF). Persisted to NVS;
 * restored at boot. Drives bluetooth_audio_set_enabled() — the audio routing gate. */
static bool s_bt_on;

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

/* Selected row, and the paint guard that remembers what is currently drawn
 * (-1 forces a repaint right after the page is rebuilt). */
static int s_setting_sel = 0;
static int s_paint_set_sel = -1;

/* Two-level view: the group list, or the items inside the selected group. The
 * same 8 row widgets are reused; only their contents differ per level. */
typedef enum { SET_VIEW_GROUPS = 0, SET_VIEW_ITEMS } settings_view_t;
static settings_view_t s_set_view = SET_VIEW_GROUPS;
static int s_set_group = 0;   /* which group the ITEMS view is showing */

/* Settings persistence: volume and log level survive reboot via NVS.
 * NVS is initialised in app_main() before ui_create(), so these helpers
 * can open the handle at any time. */
#define UI_NVS_NS      "ui_cfg"
#define UI_NVS_VOLUME  "volume"
#define UI_NVS_VOLBT   "vol_bt"
#define UI_NVS_GAIN    "gain_db"
#define UI_NVS_BT      "bt_on"
#define UI_NVS_BACKL   "backlight"
#define UI_NVS_STBY    "standby_s"

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
        hw_audio_set_speaker_volume(80);   /* default speaker volume 80% */
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
    int32_t bt = 0;
    if (nvs_get_i32(h, UI_NVS_BT, &bt) == ESP_OK) {
        s_bt_on = (bt != 0);
        bluetooth_audio_set_enabled(s_bt_on);
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

static void ui_settings_save_bt(void)
{
    nvs_handle_t h;
    if (nvs_open(UI_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, UI_NVS_BT, s_bt_on ? 1 : 0);
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

/* Debounced persistence: a setting change only marks a dirty bit and arms a
 * timer; ui_settings_flush() (called every refresh) commits once the user
 * stops tweaking, folding long-press repeats into a single NVS write. */
static uint32_t s_save_pending;
static uint32_t s_save_at_ms;

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
    if (s_save_pending & SETTINGS_DIRTY_BT) {
        ui_settings_save_bt();
    }
    if (s_save_pending & SETTINGS_DIRTY_BACKL) {
        ui_settings_save_backlight();
    }
    if (s_save_pending & SETTINGS_DIRTY_STBY) {
        ui_settings_save_standby();
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
        if (i < n) {
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
     * 重建列表 / 恢复出厂; the 蓝牙 item opens the BT screen). Inert items do nothing. */
    const setting_entry_t *e =
        &s_settings_table[s_group_items[s_set_group][s_setting_sel]];
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
    const setting_entry_t *e =
        &s_settings_table[s_group_items[s_set_group][s_setting_sel]];
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

static const char *ui_set_bt_text(void)
{
    static char buf[24];
    /* Live status: fully off, on (not linked), or linked. The persisted
     * master switch is s_bt_on; the linked state is read live so the row
     * reflects reality after a connect. */
    if (!s_bt_on) {
        snprintf(buf, sizeof(buf), "关");
    } else if (bluetooth_audio_is_connected()) {
        snprintf(buf, sizeof(buf), "已连接");
    } else {
        snprintf(buf, sizeof(buf), "开");
    }
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
    /* 1% steps within 0..10, else 5% — matches the backlight control. */
    ui_volume_step(dir);
    ui_settings_mark_dirty(SETTINGS_DIRTY_VOLUME);
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
    /* Step by one gamma level per press (HW_LCD_BACKLIGHT_STEPS levels total).
     * Each press is guaranteed to change the brightness, and equal presses
     * feel uniform to the eye. Holding the key auto-repeats this callback,
     * giving natural acceleration. The returned percentage is what we persist. */
    s_backlight = hw_lcd_step_backlight((int8_t)dir);
    ui_settings_mark_dirty(SETTINGS_DIRTY_BACKL);
}

static void ui_set_bt_lr(int dir)
{
    /* Left = off, right = on. Toggling applies the routing gate and is
     * persisted to NVS on the next flush. Switching OFF also powers the
     * Bluetooth controller fully down (if it was up) to save power. */
    s_bt_on = (dir > 0);
    bluetooth_audio_set_enabled(s_bt_on);
    if (!s_bt_on) {
        bluetooth_audio_disable();
    }
    ui_settings_mark_dirty(SETTINGS_DIRTY_BT);
    set_action(s_bt_on ? "蓝牙开" : "蓝牙关");
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

static void ui_set_bt_enter(void)
{
    /* Managing a sink needs the radio, so entering it implies BT ON — set the
     * master switch and power the controller up lazily. This keeps the
     * SETTING_BTOUT row (关/开/已连接) consistent with the live state. */
    if (!s_bt_on) {
        s_bt_on = true;
        bluetooth_audio_set_enabled(true);
        ui_settings_mark_dirty(SETTINGS_DIRTY_BT);
    }
    ui_go(UI_PAGE_BT);          /* Settings -> Bluetooth: remember parent */
}

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
