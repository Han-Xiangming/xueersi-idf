/*
 * Software layer: LVGL UI and application logic.
 * See ui.h.
 */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/param.h>

#include "board_config.h"
#include "buttons.h"
#include "audio.h"
#include "battery.h"
#include "bt_audio.h"
#include "lcd.h"
#include "sd.h"
#include "ui.h"
#include "player.h"
#include "ebook.h"

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "lvgl.h"
#include "sdkconfig.h"
#include "nvs_flash.h"
#include "nvs.h"

/* Embedded CJK bitmap font (main/fonts/lv_font_cn_16.c): ~1.2 MB, covers
 * ASCII + ~22000 Chinese ideographs + Japanese kana/kanji + CJK symbols.
 * Generated with lv_font_conv from SourceHanSansSC. Used as the UI default
 * so both static labels and dynamic text (SD/BT names) render Chinese. */
extern const lv_font_t lv_font_cn_16;

/* Single UI font: the CJK font includes Latin glyphs too, so there is no
 * need to mix multiple faces. */
#define UI_FONT (&lv_font_cn_16)

#define UI_ACTION_MSG_MS            850
/* Delay before persisting a changed setting to NVS, so a burst of key
 * repeats collapses into a single write. */
#define UI_SETTINGS_SAVE_DELAY_MS   800

static const char *const s_page_names[UI_PAGE_COUNT] = {
    "Music Player",
    "蓝牙",
    "设置",
    "电子书",
    "电子书",
};

/* Main menu lists only these pages. Bluetooth was moved into Settings as a
 * sub-page (opened from the 蓝牙 settings item), so it is no longer a
 * top-level tab in the main menu. */
static const ui_page_t s_menu_pages[] = {
    UI_PAGE_PLAYER, UI_PAGE_EBOOK_LIST, UI_PAGE_SETTINGS,
};
#define UI_MENU_PAGE_COUNT ((int)(sizeof(s_menu_pages) / sizeof(s_menu_pages[0])))

/* Settings sub-menu: up/down to select an item, left/right to change it.
 * Add new options here and they appear automatically in the list. */
typedef enum {
    SETTING_VOLUME = 0,
    SETTING_BACKLIGHT,
    SETTING_BTOUT,
    SETTING_STANDBY,
    SETTING_RESCAN,
    SETTING_RESET,
    SETTING_COUNT,
} setting_item_t;

/* Match the playlist row spacing (26px, starting at y=38) so the two
 * list-style pages line up visually. */
static const int s_setting_y[SETTING_COUNT] = {38, 64, 90, 116, 142, 168};

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
static const char *ui_set_bl_text(void);
static const char *ui_set_bt_text(void);
static const char *ui_set_sleep_text(void);
static const char *ui_set_rescan_text(void);
static const char *ui_set_reset_text(void);
static void ui_set_vol_lr(int dir);
static void ui_set_bl_lr(int dir);
static void ui_set_bt_lr(int dir);
static void ui_set_sleep_lr(int dir);
static void ui_set_rescan_enter(void);
static void ui_set_reset_enter(void);
static void ui_set_bt_enter(void);

/* A single settings row descriptor. Adding a setting = appending one row to
 * s_settings_table (and the matching SETTING_* enum). No switch/loop edits. */
typedef struct {
    const char *label;                  /* left-side label */
    const char *(*value_fn)(void);      /* right-side live value text */
    void (*on_lr)(int dir);             /* LEFT/RIGHT adjust (dir: -1/+1) */
    void (*on_enter)(void);             /* A press (NULL = not actionable) */
} setting_entry_t;

static const setting_entry_t s_settings_table[SETTING_COUNT] = {
    [SETTING_VOLUME]  = {"音量",          ui_set_vol_text,   ui_set_vol_lr,   NULL},
    [SETTING_BACKLIGHT] = {"背光",        ui_set_bl_text,    ui_set_bl_lr,    NULL},
    [SETTING_BTOUT]   = {"蓝牙",          ui_set_bt_text,    ui_set_bt_lr,    ui_set_bt_enter},
    [SETTING_STANDBY] = {"息屏",          ui_set_sleep_text, ui_set_sleep_lr, NULL},
    [SETTING_RESCAN]  = {"重建播放列表",  ui_set_rescan_text, NULL,           ui_set_rescan_enter},
    [SETTING_RESET]   = {"重置NVS",       ui_set_reset_text, NULL,           ui_set_reset_enter},
};

/* Backlight brightness (0..100 %), driven via PWM on PIN_NUM_LCD_BL.
 * Persisted to NVS; restored at boot. */
static uint8_t s_backlight = 60;

/* Bluetooth output master switch (settings page ON/OFF). Persisted to NVS;
 * restored at boot. Drives bt_audio_set_enabled() — the audio routing gate. */
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

/* Settings persistence: volume and log level survive reboot via NVS.
 * NVS is initialised in app_main() before ui_create(), so these helpers
 * can open the handle at any time. */
#define UI_NVS_NS      "ui_cfg"
#define UI_NVS_VOLUME  "volume"
#define UI_NVS_VOLBT   "vol_bt"
#define UI_NVS_BT      "bt_on"
#define UI_NVS_BACKL   "backlight"
#define UI_NVS_STBY    "standby_s"

static void ui_settings_load(void)
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
    int32_t bt = 0;
    if (nvs_get_i32(h, UI_NVS_BT, &bt) == ESP_OK) {
        s_bt_on = (bt != 0);
        bt_audio_set_enabled(s_bt_on);
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
#define SETTINGS_DIRTY_VOLUME   (1u << 0)
#define SETTINGS_DIRTY_BT       (1u << 1)
#define SETTINGS_DIRTY_BACKL    (1u << 2)
#define SETTINGS_DIRTY_STBY     (1u << 3)

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

static int s_setting_sel = 0;

#define MP3_LIST_ROWS 6
static const int s_pl_row_y[MP3_LIST_ROWS] = {38, 64, 90, 116, 142, 168};

/* Bluetooth sink picker: same 4-row list layout as the MP3 page. */
#define BT_LIST_ROWS 6
static const int s_bt_row_y[BT_LIST_ROWS] = {38, 64, 90, 116, 142, 168};
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
/* True from the moment we leave the source picker to load a (sub)folder until
 * that specific load finishes. While set, the track list deliberately shows a
 * "loading" placeholder instead of whatever stale playlist s_playlist still
 * holds — otherwise we'd flash the *previous* folder's contents for one frame
 * before the background scan publishes the new list. Cleared when the load
 * completes (player_scan_busy() goes false). */
static bool s_mp3_loading;

/* Playlist "source" picker: before showing the track list, the user picks a
 * source — <ALL> (whole card) or one top-level folder on the SD card. The
 * chosen source determines which directory player_load() scans. The track
 * ORDER within a source is fixed (sorted name) and cannot be changed at
 * runtime — there is deliberately no reorder UI; only a fresh load may change
 * it (by rescanning the filesystem). */
typedef enum {
    PV_SOURCE = 0,   /* picking a source: <ALL> + folders */
    PV_LIST,         /* browsing/playing the loaded playlist */
} player_view_t;

#define SRC_MAX 48
/* UI-only source list data (~28 KB): keep it in external PSRAM (EXT_RAM_BSS)
 * so it does not compete with the Bluetooth stack for internal DRAM. */
EXT_RAM_BSS_ATTR static char s_src_list[SRC_MAX][MP3_NAME_LEN];  /* folder names; [0]="" => <ALL> */
EXT_RAM_BSS_ATTR static char s_src_path[SRC_MAX][PLAYER_PATH_LEN];/* absolute dir path per entry */
static int      s_src_count;
static int      s_src_sel;
static player_view_t s_pv = PV_SOURCE;
static int      s_paint_src_sel = -1;

/* Marquee (scrolling) state for the selected list row. Only the highlighted
 * row scrolls when its name is wider than the line; others stay clipped. */
#define LIST_SCROLL_MS   220
#define LIST_SCROLL_GAP  8
#define LIST_LINE_W      (LCD_H_RES - 32)
typedef struct {
    int ofs;            /* current scroll offset, in characters */
    uint32_t at;        /* timestamp of last step (ms) */
    char src[MP3_NAME_LEN];
    bool scrolling;
} ui_marquee_t;
static ui_marquee_t s_mp3_mq;
static ui_marquee_t s_eb_mq;

/* Ebook book-list page: same 4-row layout as the MP3 page. */
#define EBOOK_LIST_ROWS 6
static const int s_eb_row_y[EBOOK_LIST_ROWS] = {38, 64, 90, 116, 142, 168};
static int s_eb_sel;
static char s_eb_open_name[MP3_NAME_LEN];

/* BIOS/DOS-style menu palette: dark base + cyan accent + gray monochrome text. */
static const uint32_t UI_CYAN = 0x00E0E0;
static const uint32_t UI_GRAY = 0x808080;
static const uint32_t UI_BG_DARK = 0x000000;
static const uint32_t UI_TITLE = 0xFF8000;   /* title-bar accent (orange) */

/* Main-menu layout: up to 5 list rows (first UI_MENU_PAGE_COUNT are active). */
#define UI_MENU_ROWS 5
static const int s_menu_y[UI_MENU_ROWS] = {38, 68, 98, 128, 158};

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

    lv_obj_t *battery;   /* battery body outline (persistent gauge) */
    lv_obj_t *bat_cap;   /* positive terminal cap */
    lv_obj_t *bat_seg[5]; /* 5 fill segments inside the battery icon */
    lv_obj_t *bat_text;  /* "100%" label next to the battery */

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
static uint8_t         s_ext_bat_pct = UINT8_MAX;   /* forces first paint */

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

/* Copy src into dst (at most dst_size bytes incl. NUL), never splitting a
 * multi-byte UTF-8 sequence at the end. File names from readdir() are UTF-8
 * and may have been cut mid-character by a fixed-size strncpy(), so hand
 * LVGL only complete characters; the label's CLIP mode handles pixel width. */
static void copy_utf8_clipped(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) {
        return;
    }
    const char *p = src ? src : "";
    size_t n = 0;
    while (*p && n < dst_size - 1) {
        const uint8_t b = (uint8_t)*p;
        size_t seq = 1;
        if ((b & 0xE0) == 0xC0) {
            seq = 2;
        } else if ((b & 0xF0) == 0xE0) {
            seq = 3;
        } else if ((b & 0xF8) == 0xF0) {
            seq = 4;
        }
        if (n + seq > dst_size - 1) {
            break;
        }
        size_t i = 1;
        while (i < seq && p[i] && ((uint8_t)p[i] & 0xC0) == 0x80) {
            i++;
        }
        if (i < seq) {
            break;                    /* truncated / invalid tail */
        }
        memcpy(dst + n, p, seq);
        n += seq;
        p += seq;
    }
    dst[n] = '\0';
}

/* Display-only helper: return a pointer to the name with a trailing ".xxx"
 * extension (e.g. ".mp3") removed. Does not mutate the source; the returned
 * pointer is into a static buffer (single-slot, fine for our one-shot use).
 * Storage keeps the real name (player_play builds the path from it). */
static const char *strip_ext(const char *name)
{
    static char s_buf[MP3_NAME_LEN];
    size_t n = strnlen(name, MP3_NAME_LEN);
    if (n > 0 && n < MP3_NAME_LEN) {
        s_buf[n] = '\0';
        memcpy(s_buf, name, n);
        /* strip last extension */
        for (size_t i = n; i > 0; i--) {
            if (s_buf[i - 1] == '.') {
                s_buf[i - 1] = '\0';
                break;
            }
            if (s_buf[i - 1] == '/') {
                break;
            }
        }
        return s_buf;
    }
    return name;
}

/* UTF-8-aware pixel width estimate for a label using the default font.
 * CJK (3-byte) sequences count as 16px, everything else (ASCII + Latin
 * extensions, 1-2 bytes) as 8px. Good enough to decide whether a list entry
 * needs to scroll. */
static int ui_text_px_width(const char *text)
{
    int w = 0;
    const uint8_t *p = (const uint8_t *)text;
    while (*p) {
        uint8_t b = *p;
        if ((b & 0xE0) == 0xC0) {       /* 2-byte */
            p += 2; w += 8;
        } else if ((b & 0xF0) == 0xE0) {/* 3-byte (CJK) */
            p += 3; w += 16;
        } else if ((b & 0xF8) == 0xF0) {/* 4-byte */
            p += 4; w += 16;
        } else {                        /* 1-byte (ASCII) */
            p += 1; w += 8;
        }
    }
    return w;
}

/* Advance the marquee for the selected row. Returns true and fills `out` with
 * the shifted string "src[ofs:] + gap + src[0:ofs]" when scrolling; the caller
 * should keep refreshing until it returns false (name fits / finished a loop
 * and is now static). */
static bool ui_marquee_step(ui_marquee_t *mq, char *out, size_t out_size,
                            const char *name)
{
    if (name == NULL) {
        name = "";
    }
    int w = ui_text_px_width(name);
    if (w <= LIST_LINE_W) {
        mq->scrolling = false;
        mq->ofs = 0;
        snprintf(out, out_size, "%s", name);
        return false;
    }
    /* Need to scroll. */
    if (mq->src[0] == '\0' || strncmp(mq->src, name, MP3_NAME_LEN) != 0) {
        /* New/changed name: (re)start from the beginning. */
        strncpy(mq->src, name, MP3_NAME_LEN - 1);
        mq->src[MP3_NAME_LEN - 1] = '\0';
        mq->ofs = 0;
    }
    mq->scrolling = true;
    uint32_t now = lv_tick_get();
    if (now - mq->at >= LIST_SCROLL_MS) {
        mq->at = now;
        mq->ofs++;
    }
    int len = (int)strlen(mq->src);
    if (mq->ofs >= len) {
        mq->ofs = 0;                    /* loop back to start */
    }
    /* Build "tail + gap + head" so the name circulates. */
    char gap[LIST_SCROLL_GAP + 1];
    memset(gap, ' ', LIST_SCROLL_GAP);
    gap[LIST_SCROLL_GAP] = '\0';
    int tail = len - mq->ofs;
    snprintf(out, out_size, "%.*s%s%.*s",
             tail, mq->src + mq->ofs, gap,
             mq->ofs, mq->src);
    return true;
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
    lv_obj_set_pos(bar, 18, 120);
    lv_obj_set_size(bar, 284, 8);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, value, LV_ANIM_OFF);
    lv_obj_set_style_radius(bar, 4, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_GRAY), 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_CYAN), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
    return bar;
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

/* ------------------------------------------------------------------ */
/* Settings table callbacks                                           */
/* ------------------------------------------------------------------ */
/* Forward declaration: ui_enter_page is defined further below. */
static void ui_enter_page(ui_page_t page);

/* Each entry's live value text. Returned strings live in function-local
 * static buffers — safe because the UI is driven by a single task. */

static const char *ui_set_vol_text(void)
{
    static char buf[24];
    snprintf(buf, sizeof(buf), "%u%%", (unsigned)hw_audio_get_volume());
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
    } else if (bt_audio_is_connected()) {
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
        snprintf(buf, sizeof(buf), "永不");
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
    snprintf(buf, sizeof(buf), s_cache_present ? "有缓存" : "无缓存");
    return buf;
}

static const char *ui_set_reset_text(void)
{
    return "按A还原";
}

/* Left/right adjust callbacks. Each owns its dirty-mark + set_action. */

static void ui_set_vol_lr(int dir)
{
    /* 1% steps below 10% for fine control, else 10%. */
    ui_volume_step(dir, 10);
    ui_settings_mark_dirty(SETTINGS_DIRTY_VOLUME);
    set_action(ui_set_vol_text());
}

static void ui_set_bl_lr(int dir)
{
    /* Same stepping as volume: 1% steps at/below 10% (down), else 10%. */
    int v = (int)s_backlight;
    int step = (v < 10 || (v == 10 && dir < 0)) ? 1 : 10;
    v += dir * step;
    /* Snap coarse upward moves onto multiples of 10 once out of the fine
     * zone (9% + coarse -> 10%), keeping the scale tidy like volume. */
    if (step == 1 && dir > 0 && v > 10) {
        v = 10;
    }
    s_backlight = (uint8_t)MAX(0, MIN(v, 100));
    hw_lcd_set_backlight(s_backlight);
    ui_settings_mark_dirty(SETTINGS_DIRTY_BACKL);
    set_action(ui_set_bl_text());
}

static void ui_set_bt_lr(int dir)
{
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
}

static void ui_set_sleep_lr(int dir)
{
    int opt = (int)s_standby_opt + dir;
    opt = MAX(0, MIN(opt, (int)STANDBY_OPT_COUNT - 1));
    s_standby_opt = (standby_opt_t)opt;
    hw_lcd_set_standby_timeout((uint32_t)s_standby_opts[s_standby_opt] * 1000);
    ui_settings_mark_dirty(SETTINGS_DIRTY_STBY);
    set_action(ui_set_sleep_text());
}

/* A-press (enter) callbacks. */

static void ui_set_bt_enter(void)
{
    /* Managing a sink needs the radio, so entering it implies BT ON — set the
     * master switch and power the controller up lazily. This keeps the
     * SETTING_BTOUT row (关/开/已连接) consistent with the live state. */
    if (!s_bt_on) {
        s_bt_on = true;
        bt_audio_set_enabled(true);
        ui_settings_mark_dirty(SETTINGS_DIRTY_BT);
    }
    ui_enter_page(UI_PAGE_BT);
}

static void ui_set_rescan_enter(void)
{
    /* Drop the on-card playlist cache and rebuild it from a fresh, full-card
     * scan. The scan is asynchronous; the player falls back to a real scan
     * whenever the cache is absent, so this is safe even while playing. */
    player_rescan();
    set_action("重建中...");
}

static void ui_set_reset_enter(void)
{
    /* Restore NVS to factory defaults: wipe the whole NVS partition and
     * reboot. Boot will re-create every setting at its default. */
    set_action("重置中...");
    ui_refresh();
    nvs_flash_erase();
    esp_restart();
}

static void ui_build_settings(lv_obj_t *page)
{
    s_setting_sel = 0;

    for (int i = 0; i < SETTING_COUNT; i++) {
        lv_obj_t *cur = lv_label_create(page);
        lv_label_set_text(cur, " ");
        lv_obj_set_pos(cur, 8, s_setting_y[i]);
        lv_obj_set_style_text_font(cur, &lv_font_cn_16, 0);
        lv_obj_set_style_text_color(cur, lv_color_hex(UI_GRAY), 0);
        s_ui.set_cursor[i] = cur;

        /* Fixed left-aligned label; the live value lives in its own column
         * (set_value) so items with different label lengths still line up
         * regardless of the (non-monospaced) byte width of the UTF-8 text. */
        lv_obj_t *txt = lv_label_create(page);
        lv_label_set_text(txt, s_settings_table[i].label);
        lv_obj_set_pos(txt, 18, s_setting_y[i]);
        lv_obj_set_style_text_font(txt, &lv_font_cn_16, 0);
        lv_obj_set_style_text_color(txt, lv_color_hex(UI_GRAY), 0);
        s_ui.set_text[i] = txt;

        lv_obj_t *val = lv_label_create(page);
        lv_label_set_long_mode(val, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_size(val, 80, LV_SIZE_CONTENT);
        lv_obj_set_pos(val, 232, s_setting_y[i]);
        lv_obj_set_style_text_font(val, &lv_font_cn_16, 0);
        lv_obj_set_style_text_color(val, lv_color_hex(UI_GRAY), 0);
        lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);
        s_ui.set_value[i] = val;
    }

    s_ui.hint = ui_label(page, "上/下选 A进入 左/右设 B返回", 204,
                         UI_GRAY, &lv_font_cn_16, LV_TEXT_ALIGN_CENTER);
}

/* Music root: playlist sources are the FOLDERS directly under /sdcard/Music.
 * Each becomes one selectable source; <ALL> scans the whole Music tree. */
#define MUSIC_ROOT PLAYER_ROOT "/Music"

/* Discover playlist sources: <ALL> (every .mp3 under /sdcard/Music, recursive)
 * plus each sub-directory directly under /sdcard/Music (one source each).
 * Fills s_src_list[] / s_src_path[]; entry 0 is the whole-Music pseudo-source.
 * Re-run each time the player page is (re)built so a newly added folder shows
 * up. */
static void ui_discover_sources(void)
{
    s_src_count = 0;
    /* Entry 0: all of /sdcard/Music (recursive). */
    s_src_list[0][0] = '\0';                 /* empty name => render as "<ALL>" */
    snprintf(s_src_path[0], sizeof(s_src_path[0]), "%s", MUSIC_ROOT);

    DIR *d = opendir(MUSIC_ROOT);
    if (d != NULL) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL && s_src_count < SRC_MAX - 1) {
            const char *fn = e->d_name;
            if (fn[0] == '.') {
                continue;
            }
            char child[PLAYER_PATH_LEN];
            snprintf(child, sizeof(child), "%s/%s", MUSIC_ROOT, fn);
            struct stat st;
            if (stat(child, &st) != 0 || !S_ISDIR(st.st_mode)) {
                continue;                    /* only sub-directories are sources */
            }
            int idx = s_src_count + 1;
            /* snprintf (not strncpy) so the buffer is always NUL-terminated and
             * GCC's -Wstringop-truncation stays quiet (fn is d_name, 255 bytes). */
            snprintf(s_src_list[idx], sizeof(s_src_list[idx]), "%s", fn);
            snprintf(s_src_path[idx], sizeof(s_src_path[idx]), "%s", child);
            s_src_count = idx;
        }
        closedir(d);
    }
    s_src_count = (s_src_count == 0) ? 1 : s_src_count + 1;  /* ensure <ALL> present */
    if (s_src_sel >= s_src_count) {
        s_src_sel = 0;
    }
}

static void ui_build_player(lv_obj_t *page)
{
    s_mp3_sel = 0;
    s_src_sel = 0;
    s_pv = PV_SOURCE;
    ui_discover_sources();

    for (int i = 0; i < MP3_LIST_ROWS; i++) {
        lv_obj_t *cur = lv_label_create(page);
        lv_label_set_text(cur, " ");
        lv_obj_set_pos(cur, 6, s_pl_row_y[i]);
        lv_obj_set_style_text_font(cur, &lv_font_cn_16, 0);
        lv_obj_set_style_text_color(cur, lv_color_hex(UI_CYAN), 0);
        s_ui.pl_cursor[i] = cur;

        lv_obj_t *txt = lv_label_create(page);
        lv_label_set_long_mode(txt, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_size(txt, LCD_H_RES - 32, LV_SIZE_CONTENT);
        lv_obj_set_pos(txt, 16, s_pl_row_y[i]);
        lv_obj_set_style_text_font(txt, &lv_font_cn_16, 0);
        lv_obj_set_style_text_color(txt, lv_color_hex(UI_GRAY), 0);
        s_ui.pl_text[i] = txt;
    }

    s_ui.pl_prog = ui_label(page, "选择来源", 196, UI_GRAY, &lv_font_cn_16, LV_TEXT_ALIGN_CENTER);
    s_ui.hint = ui_label(page, "上/下选 A进入 B返回", 214, UI_GRAY, &lv_font_cn_16, LV_TEXT_ALIGN_CENTER);
}

/* Bluetooth page: entering it kicks off a scan; the list fills live. */
static void ui_build_bt(lv_obj_t *page)
{
    s_bt_sel = 0;

    for (int i = 0; i < BT_LIST_ROWS; i++) {
        lv_obj_t *cur = lv_label_create(page);
        lv_label_set_text(cur, " ");
        lv_obj_set_pos(cur, 6, s_bt_row_y[i]);
        lv_obj_set_style_text_font(cur, &lv_font_cn_16, 0);
        lv_obj_set_style_text_color(cur, lv_color_hex(UI_CYAN), 0);
        s_ui.bt_cursor[i] = cur;

        lv_obj_t *txt = lv_label_create(page);
        lv_label_set_long_mode(txt, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_size(txt, LCD_H_RES - 32, LV_SIZE_CONTENT);
        lv_obj_set_pos(txt, 16, s_bt_row_y[i]);
        lv_obj_set_style_text_font(txt, &lv_font_cn_16, 0);
        lv_obj_set_style_text_color(txt, lv_color_hex(UI_GRAY), 0);
        s_ui.bt_text[i] = txt;
    }

    s_ui.bt_status = ui_label(page, "扫描中...", 196, UI_GRAY,
                              &lv_font_cn_16, LV_TEXT_ALIGN_CENTER);
    s_ui.hint = ui_label(page, "上/下选 A连接 B返回", 214, UI_GRAY,
                         &lv_font_cn_16, LV_TEXT_ALIGN_CENTER);

    bt_audio_scan_start();
}

/* Book display name. Keep the full filename (including the ".txt" suffix) so
 * the file list and the reader title show the extension consistently. */
static void copy_book_name(char *dst, size_t dst_size, const char *src)
{
    snprintf(dst, dst_size, "%s", src ? src : "");
}

static void ui_build_ebook_list(lv_obj_t *page)
{
    s_eb_sel = 0;

    for (int i = 0; i < EBOOK_LIST_ROWS; i++) {
        lv_obj_t *cur = lv_label_create(page);
        lv_label_set_text(cur, " ");
        lv_obj_set_pos(cur, 6, s_eb_row_y[i]);
        lv_obj_set_style_text_font(cur, &lv_font_cn_16, 0);
        lv_obj_set_style_text_color(cur, lv_color_hex(UI_CYAN), 0);
        s_ui.eb_cursor[i] = cur;

        lv_obj_t *txt = lv_label_create(page);
        lv_label_set_long_mode(txt, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_size(txt, LCD_H_RES - 32, LV_SIZE_CONTENT);
        lv_obj_set_pos(txt, 16, s_eb_row_y[i]);
        lv_obj_set_style_text_font(txt, &lv_font_cn_16, 0);
        lv_obj_set_style_text_color(txt, lv_color_hex(UI_GRAY), 0);
        s_ui.eb_text[i] = txt;
    }

    s_ui.eb_status = ui_label(page, "扫描中...", 196, UI_GRAY,
                              &lv_font_cn_16, LV_TEXT_ALIGN_CENTER);
    s_ui.hint = ui_label(page, "上/下选 A打开 B返回", 214, UI_GRAY,
                         &lv_font_cn_16, LV_TEXT_ALIGN_CENTER);

    ebook_scan_start();
}

static void ui_build_ebook_read(lv_obj_t *page)
{
    /* Single body label: the reader engine joins exactly 8 lines with '\n'
     * and measures with the same font, so the layout matches exactly. The
     * 16 px font's natural line height is 30 px; we compress it with a
     * negative line-space so the effective row height becomes 22 px and the
     * widget needs 8*22 = 176 px (y=36..212, just above the y=214 status bar). */
    lv_obj_t *txt = lv_label_create(page);
    lv_label_set_long_mode(txt, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_pos(txt, 0, 36);
    lv_obj_set_size(txt, 320, 176);
    lv_obj_set_style_text_font(txt, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(txt, lv_color_hex(UI_GRAY), 0);
    lv_obj_set_style_text_line_space(txt, -8, 0);
    s_ui.eb_text_label = txt;

    /* Status row: wide text progress bar at the left (28 cells) and the
     * percentage right-aligned to the screen's right edge. The centered hint
     * label below is used only by toasts (see ui_set_hint). The status row
     * sits 20 px below the 6-line body (body ends at y=168 -> y=188) so it is
     * clearly separated; compress line space to keep it a slim one-line strip.
     * The hint row sits just under it. */
    lv_obj_t *bar = lv_label_create(page);
    lv_label_set_text(bar, "[----------------------------]");
    lv_label_set_long_mode(bar, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_pos(bar, 8, 214);
    lv_obj_set_style_text_font(bar, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(bar, lv_color_hex(UI_GRAY), 0);
    lv_obj_set_style_text_line_space(bar, -8, 0);
    s_ui.eb_bar = bar;

    lv_obj_t *pct = lv_label_create(page);
    lv_label_set_text(pct, "0%");
    lv_label_set_long_mode(pct, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_pos(pct, 272, 214);
    lv_obj_set_size(pct, 40, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(pct, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(pct, lv_color_hex(UI_GRAY), 0);
    lv_obj_set_style_text_align(pct, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_line_space(pct, -8, 0);
    s_ui.eb_pct = pct;

    s_ui.hint = ui_label(page, "", 214, UI_GRAY, &lv_font_cn_16,
                         LV_TEXT_ALIGN_CENTER);
}

static void ui_build_page_content(lv_obj_t *page)
{
    s_ui.title = ui_label(page, s_page_names[s_ui.page_id], 2, UI_TITLE, &lv_font_cn_16, LV_TEXT_ALIGN_LEFT);
    /* Header row sits 2px left of the default so the whole top bar reads as
     * one unit, clear of the battery overlay. */
    lv_obj_set_pos(s_ui.title, 6, 2);
    /* Top-right status label. Secondary pages leave it empty; the Player page
     * fills it with the playback state (>> / || / --) in ui_refresh(). The
     * top-right corner (x >= 250) is claimed by the persistent battery gauge
     * drawn above the page container, so the status text must end before it:
     * cap the width at 238 (right edge x=246, 4px clear of the battery). */
    s_ui.status = ui_label(page, "", 2, UI_GRAY, &lv_font_cn_16, LV_TEXT_ALIGN_RIGHT);
    lv_obj_set_width(s_ui.status, 238);

    /* Header separator, matching the main-menu style. */
    lv_obj_t *sep = lv_obj_create(page);
    lv_obj_remove_style_all(sep);
    lv_obj_set_pos(sep, 0, 34);
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
        lv_obj_set_width(s_ui.title, 200);
        ui_build_ebook_read(page);
        return;
    }

    /* Generic value/bar page (used by the SD CARD page). */
    s_ui.value = ui_label(page, "--", 50, UI_CYAN, &lv_font_cn_16, LV_TEXT_ALIGN_CENTER);
    s_ui.sub = ui_label(page, "--", 88, UI_GRAY, &lv_font_cn_16, LV_TEXT_ALIGN_CENTER);
    s_ui.bar = ui_bar(page, 0);
    s_ui.hint = ui_label(page, "A重扫 B返回", 176, UI_GRAY, &lv_font_cn_16, LV_TEXT_ALIGN_CENTER);
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
    lv_obj_set_style_text_font(title, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_TITLE), 0);

    /* Separator line spanning the full width, clear of the title. */
    lv_obj_t *sep = lv_obj_create(mp);
    lv_obj_remove_style_all(sep);
    lv_obj_set_pos(sep, 0, 34);
    lv_obj_set_size(sep, LCD_H_RES, 1);
    lv_obj_set_style_bg_color(sep, lv_color_hex(UI_GRAY), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);

    /* Menu rows: cursor at x=8, text at x=18. */
    for (int i = 0; i < UI_MENU_ROWS; i++) {
        lv_obj_t *cur = lv_label_create(mp);
        lv_label_set_text(cur, " ");
        lv_obj_set_pos(cur, 8, s_menu_y[i]);
        lv_obj_set_style_text_font(cur, &lv_font_cn_16, 0);
        lv_obj_set_style_text_color(cur, lv_color_hex(UI_GRAY), 0);
        s_ui.menu_cursor[i] = cur;

        lv_obj_t *txt = lv_label_create(mp);
        lv_label_set_long_mode(txt, LV_LABEL_LONG_MODE_CLIP);
        lv_label_set_text(txt, i < UI_MENU_PAGE_COUNT ? s_page_names[s_menu_pages[i]] : "");
        lv_obj_set_pos(txt, 18, s_menu_y[i]);
        lv_obj_set_style_text_font(txt, &lv_font_cn_16, 0);
        lv_obj_set_style_text_color(txt, lv_color_hex(UI_GRAY), 0);
        s_ui.menu_text[i] = txt;
    }

    /* Bottom status bar: "[n/3]" left, "A:OK B:BK" right, both gray. */
    char mbuf[32];
    snprintf(mbuf, sizeof(mbuf), "[1/%d]", (int)UI_MENU_PAGE_COUNT);
    s_ui.menu_status = lv_label_create(mp);
    lv_label_set_text(s_ui.menu_status, mbuf);
    lv_obj_set_pos(s_ui.menu_status, 4, 206);
    lv_obj_set_style_text_font(s_ui.menu_status, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(s_ui.menu_status, lv_color_hex(UI_GRAY), 0);

    lv_obj_t *hint = lv_label_create(mp);
    lv_label_set_text(hint, "A:OK B:BK");
    lv_obj_set_pos(hint, 200, 206);
    lv_obj_set_style_text_font(hint, &lv_font_cn_16, 0);
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
    /* Entering the settings page: force a fresh cache-existence query on the
     * next refresh (the cached value may be stale from a previous visit). */
    if (page == UI_PAGE_SETTINGS) {
        s_cache_queried_ver = 0;
    }
    /* Entering the Music Player: prefer the on-card playlist cache so the list
     * shows instantly. Only falls back to a real scan (which rewrites the
     * cache) when no cache exists — never blocks the UI on the FATFS walk. */
    if (page == UI_PAGE_PLAYER) {
        player_scan_with_cache();
        /* If the cache was missing, a background scan is now in flight.
         * Suppress any stale list in the track view until it publishes, so we
         * don't flash the previous folder's contents. (The source picker is
         * unaffected; s_mp3_loading only gates the PV_LIST rows.) */
        if (player_scan_busy()) {
            s_mp3_loading = true;
        }
    }
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
        /* Re-discover sources and refresh the current load so the picker and
         * the track list reflect the new card state. */
        if (s_ui.page_id == UI_PAGE_PLAYER) {
            ui_discover_sources();
            s_paint_src_sel = -1;
        }
        player_scan_with_cache();   /* prefer cache; scan only if absent */
        /* A background scan may now be running; if we're showing the track
         * list, suppress the stale rows until it publishes (same guard as
         * entering a sub-folder). */
        if (s_pv == PV_LIST && player_scan_busy()) {
            s_mp3_loading = true;
        }
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

    /* Battery level is sampled once per second in the background; refresh the
     * gauge whenever the percentage moves. */
    uint8_t bp = hw_battery_percent();
    if (bp != s_ext_bat_pct) {
        s_ext_bat_pct = bp;
        changed = true;
    }
    return changed;
}

/* 5-color appearance scheme: one color per battery-level band. The whole
 * gauge (body, cap, filled segments and text) takes the band color so a low
 * battery reads as an obvious red icon.
 *   <=15% red | <=35% orange | <=60% yellow | <=85% cyan | <=100% green */
static const uint32_t s_bat_palette[5] = {
    0xFF2020,  /* red    - critical (<=15%)  */
    0xFF9000,  /* orange - low      (<=35%)  */
    0xFFE000,  /* yellow - medium   (<=60%)  */
    0x00E0FF,  /* cyan   - good      (<=85%)  */
    0x20FF40,  /* green  - full      (<=100%) */
};

static uint32_t bat_color_for_pct(uint8_t pct)
{
    if (pct <= 15)  return s_bat_palette[0];
    if (pct <= 35)  return s_bat_palette[1];
    if (pct <= 60)  return s_bat_palette[2];
    if (pct <= 85)  return s_bat_palette[3];
    return s_bat_palette[4];
}

/* Update the persistent top-right battery gauge: re-paint the 5-segment fill
 * and the "100%" label in the band color. "--" stays until first sample. */
static void ui_refresh_battery(void)
{
    if (!s_ui.battery) {
        return;
    }

    uint8_t pct = hw_battery_percent();
    float vbat = hw_battery_voltage();

    /* Pre-sample: no data yet. */
    if (pct == 0 && vbat <= 0.0f) {
        for (int i = 0; i < 5; ++i) {
            lv_obj_set_style_bg_opa(s_ui.bat_seg[i], LV_OPA_TRANSP, 0);
        }
        lv_label_set_text(s_ui.bat_text, "--%");
        return;
    }

    const uint32_t color = bat_color_for_pct(pct);

    /* Linear mapping: 0..100% → 0..5 filled segments. */
    uint8_t filled = (uint8_t)((pct * 5 + 50) / 100); /* round to nearest */
    if (filled > 5) {
        filled = 5;
    }
    for (int i = 0; i < 5; ++i) {
        lv_obj_t *seg = s_ui.bat_seg[i];
        if (i < filled) {
            lv_obj_set_style_bg_color(seg, lv_color_hex(color), 0);
            lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
        }
        else {
            lv_obj_set_style_bg_opa(seg, LV_OPA_TRANSP, 0);
        }
    }

    /* Body outline, terminal cap and text all take the band color. */
    lv_obj_set_style_border_color(s_ui.battery, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(s_ui.bat_cap, lv_color_hex(color), 0);
    lv_obj_set_style_text_color(s_ui.bat_text, lv_color_hex(color), 0);

    char buf[8];
    snprintf(buf, sizeof(buf), "%u%%", (unsigned)pct);
    lv_label_set_text(s_ui.bat_text, buf);

    /* #9 Low-battery banner: when the cell is critically low the battery icon
     * is already red; surface a plain-text hint on the bottom row too. This
     * runs last in ui_refresh() (after each page's own hint), so it overrides
     * the page hint only while the battery is critical. */
    if (pct != 0 && pct <= 15 && s_ui.hint != NULL) {
        ui_label_set(s_ui.hint, "低电量");
    }
}

void ui_refresh(void)
{
    ui_settings_flush();
    if (s_in_menu) {
        /* Menu is event-driven, but keep the battery gauge live on every tick. */
        ui_refresh_battery();
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
    case UI_PAGE_SETTINGS: {
        const bool sel_changed = (s_setting_sel != s_paint_set_sel);
        /* Re-query cache existence only when the playlist scan version
         * changes, not every frame — avoids hammering FATFS stat() while the
         * toast is showing and makes scrolling feel responsive. */
        const uint32_t scan_ver = player_scan_version();
        if (scan_ver != s_cache_queried_ver) {
            s_cache_present = player_cache_exists();
            s_cache_queried_ver = scan_ver;
        }
        for (int i = 0; i < SETTING_COUNT; i++) {
            const int sel = (i == s_setting_sel);
            ui_label_set(s_ui.set_cursor[i], sel ? ">" : " ");
            /* Set the highlight color every frame (not just on sel_changed):
             * the list can be refreshed/rebuilt underneath us (e.g. cache load
             * on player entry) and a sel_changed-gated repaint leaves a stale
             * CYAN highlight on the wrong row. Cheap for a handful of rows. */
            lv_obj_set_style_text_color(s_ui.set_cursor[i], lv_color_hex(UI_CYAN), 0);
            lv_obj_set_style_text_color(s_ui.set_text[i],
                                        lv_color_hex(sel ? UI_CYAN : UI_GRAY), 0);
            lv_obj_set_style_text_color(s_ui.set_value[i],
                                        lv_color_hex(sel ? UI_CYAN : UI_GRAY), 0);
            const setting_entry_t *e = &s_settings_table[i];
            const char *txt = e->value_fn ? e->value_fn() : "";
            ui_label_set(s_ui.set_value[i], txt);
        }
        if (sel_changed) {
            s_paint_set_sel = s_setting_sel;
        }
        ui_set_hint("上/下选 A进入 B返回");
        break;
    }
    case UI_PAGE_PLAYER: {
        if (s_pv == PV_SOURCE) {
            /* Source picker: <ALL> (whole card) + top-level folders. */
            int top = s_src_sel - 1;
            if (top < 0) {
                top = 0;
            }
            if (top > s_src_count - MP3_LIST_ROWS) {
                top = MAX(0, s_src_count - MP3_LIST_ROWS);
            }
            const bool sel_changed = (s_src_sel != s_paint_src_sel)
                                   || (top != s_paint_mp3_top);
            for (int i = 0; i < MP3_LIST_ROWS; i++) {
                int idx = top + i;
                const int sel = (idx == s_src_sel);
                if (idx < s_src_count) {
                    static char s_src_buf[MP3_NAME_LEN];
                    const char *label = (s_src_list[idx][0] == '\0')
                                      ? "<ALL>" : s_src_list[idx];
                    copy_utf8_clipped(s_src_buf, sizeof(s_src_buf), label);
                    ui_label_set(s_ui.pl_cursor[i], sel ? ">" : " ");
                    /* Repaint the highlight every frame: the source/cache list
                     * can change underneath us (player entry loads the cache
                     * synchronously), and a sel_changed-gated repaint would
                     * leave a stale CYAN highlight on the wrong row. */
                    lv_obj_set_style_text_color(s_ui.pl_cursor[i],
                                                lv_color_hex(UI_CYAN), 0);
                    lv_obj_set_style_text_color(s_ui.pl_text[i],
                        lv_color_hex(sel ? UI_CYAN : UI_GRAY), 0);
                    ui_label_set(s_ui.pl_text[i], s_src_buf);
                }
                else {
                    ui_label_set(s_ui.pl_cursor[i], " ");
                    ui_label_set(s_ui.pl_text[i], "");
                }
            }
            if (sel_changed) {
                s_paint_src_sel = s_src_sel;
                s_paint_mp3_top = top;
            }
            ui_label_set(s_ui.status, "--");
            ui_label_set(s_ui.pl_prog, "选择播放来源");
            ui_set_hint("上/下选 A进入 B返回");
            break;
        }

        /* PV_LIST: the track list (published by the background load). Re-fetch
         * each pass so a completed load shows up without a page rebuild. */
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
        /* While a (sub)folder load is in flight, s_playlist still holds the
         * previous folder's contents. Suppress drawing it so we don't flash
         * the old list for a frame before the scan publishes the new one. */
        const bool loading = s_mp3_loading && player_scan_busy();
        for (int i = 0; i < MP3_LIST_ROWS; i++) {
            int idx = top + i;
            const int sel = (idx == s_mp3_sel);
            if (!loading && idx < s_mp3_count) {
                /* Static scratch buffer: this runs every UI refresh (16 ms). The
                 * lvgl task stack is only 10 KB, so a 256-byte stack array here
                 * overflows it under load (BT+SDSPI IRQs) and corrupts adjacent
                 * memory — surfacing as a SPI ISR Guru Meditation. */
                static char s_pl_name_buf[MP3_NAME_LEN];
                if (sel) {
                    /* Selected row scrolls its name if wider than the line — but
                     * only when idle. While playing we freeze a static (clipped)
                     * name to avoid needless redraws and save resources. */
                    if (player_state() == PLAYER_PLAYING) {
                        s_mp3_mq.scrolling = false;
                        copy_utf8_clipped(s_pl_name_buf, sizeof(s_pl_name_buf),
                                          strip_ext(player_scan_name(idx)));
                    }
                    else {
                        ui_marquee_step(&s_mp3_mq, s_pl_name_buf,
                                        sizeof(s_pl_name_buf),
                                        strip_ext(player_scan_name(idx)));
                    }
                }
                else {
                    copy_utf8_clipped(s_pl_name_buf, sizeof(s_pl_name_buf),
                                      strip_ext(player_scan_name(idx)));
                    /* Do NOT clear s_mp3_mq.scrolling here: the selected row may
                     * be drawn earlier in this loop, and a later non-selected row
                     * would clobber it to false, freezing the marquee after one
                     * step. ui_marquee_step() already sets scrolling=false when
                     * the (selected) name fits the line. */
                }
                ui_label_set(s_ui.pl_cursor[i], sel ? ">" : " ");
                /* Repaint the highlight every frame: the track list can be
                 * refreshed/rebuilt underneath us (cache load on player entry,
                 * or a live scan), and a sel_changed-gated repaint leaves a
                 * stale CYAN highlight on the wrong row. */
                lv_obj_set_style_text_color(s_ui.pl_cursor[i], lv_color_hex(UI_CYAN), 0);
                lv_obj_set_style_text_color(s_ui.pl_text[i],
                                            lv_color_hex(sel ? UI_CYAN : UI_GRAY), 0);
                ui_label_set(s_ui.pl_text[i], s_pl_name_buf);
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
        /* The folder load we armed on entering this view has now published its
         * list (busy cleared) — stop suppressing the track rows. */
        if (s_mp3_loading && !player_scan_busy()) {
            s_mp3_loading = false;
        }
        player_state_t st = player_state();
        /* Top-right status: playback symbol + repeat mode, e.g. ">>单曲" /
         * "||列表" / "--列表", so the current loop mode is always visible. */
        char stbuf[16];
        snprintf(stbuf, sizeof(stbuf), "%s%s",
                 st == PLAYER_PLAYING ? ">>" : st == PLAYER_PAUSED ? "||" : "--",
                 player_repeat_mode() == PLAYER_REPEAT_ONE ? "单曲" : "列表");
        ui_label_set(s_ui.status, stbuf);
        if (st == PLAYER_IDLE) {
            if (player_scan_busy()) {
                ui_label_set(s_ui.pl_prog, "加载中...");
            }
            else {
                ui_label_set(s_ui.pl_prog, s_mp3_count
                             ? (player_repeat_mode() == PLAYER_REPEAT_ONE
                                ? "循环:单曲" : "循环:列表")
                             : "无MP3文件");
            }
        }
        else {
            /* Now-playing line: source name + track name (no extension). */
            char prog[28];
            snprintf(prog, sizeof(prog), "[%s]%s",
                     player_current_src_name(),
                     strip_ext(player_current_name()));
            prog[27] = '\0';
            ui_label_set(s_ui.pl_prog, prog);
        }
        if (st == PLAYER_PLAYING) {
            ui_set_hint("左/右切歌 上/下音量 A暂停 Select循环");
        }
        else if (st == PLAYER_PAUSED) {
            ui_set_hint("左/右切歌 上/下音量 A继续 Select循环");
        }
        else {
            ui_set_hint("左/右切歌 上/下选择 A播放 Select循环");
        }
        /* Sticky playback-error toast: while an error is set and nothing is
         * playing, re-arm the toast every refresh so the hint row keeps
         * showing it (e.g. "文件损坏") instead of the normal hint, until the
         * next successful play clears it. */
        if (st == PLAYER_IDLE && player_last_error() != PLAYER_ERR_NONE) {
            set_action(player_err_text(player_last_error()));
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
                /* Paint the row color every refresh so the first paint after
                 * entering shows the selected row in cyan. */
                lv_obj_set_style_text_color(s_ui.bt_text[i],
                                            lv_color_hex(sel ? UI_CYAN : UI_GRAY), 0);
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
            /* Connected sink takes over output: explicitly flip the route so
             * decoded audio goes to Bluetooth instead of the speaker. */
            hw_audio_set_route(AUDIO_ROUTE_BT);
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
                static char s_eb_name_buf[64];
                copy_book_name(s_eb_name_buf, sizeof(s_eb_name_buf),
                               ebook_scan_name(idx));
                if (sel) {
                    /* Selected row scrolls its name if wider than the line. */
                    ui_marquee_step(&s_eb_mq, s_eb_name_buf,
                                    sizeof(s_eb_name_buf), s_eb_name_buf);
                }
                else {
                    /* Same as the MP3 list: never clobber s_eb_mq.scrolling from
                     * a non-selected row (would freeze the marquee after one step).
                     * ui_marquee_step() clears it when the selected name fits. */
                }
                ui_label_set(s_ui.eb_cursor[i], sel ? ">" : " ");
                /* Paint the row color every refresh (not only on selection
                 * change) so the first paint after entering the page shows the
                 * selected row in cyan. */
                lv_obj_set_style_text_color(s_ui.eb_cursor[i],
                                            lv_color_hex(UI_CYAN), 0);
                lv_obj_set_style_text_color(s_ui.eb_text[i],
                                            lv_color_hex(sel ? UI_CYAN : UI_GRAY), 0);
                ui_label_set(s_ui.eb_text[i], s_eb_name_buf);
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

        /* Status row: 28-cell text progress bar (left) + percentage (right). */
        int pct = ebook_percent();
        char prog[32];
        prog[0] = '[';
        int done = (pct * 28 + 50) / 100;   /* rounded to the nearest cell */
        for (int i = 0; i < 28; i++) {
            prog[1 + i] = (i < done) ? '=' : '-';
        }
        prog[29] = ']';
        prog[30] = '\0';
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
    ui_refresh_battery();
    /* Keep refreshing while a selected list row is still scrolling, otherwise
     * the marquee freezes after a single step (s_ui_dirty would be cleared).
     * During playback the MP3 marquee is intentionally frozen, so it must not
     * force refreshes. */
    if ((s_ui.page_id == UI_PAGE_PLAYER && s_mp3_mq.scrolling
         && player_state() != PLAYER_PLAYING) ||
        (s_ui.page_id == UI_PAGE_EBOOK_LIST && s_eb_mq.scrolling)) {
        s_ui_dirty = true;
    }
    else {
        s_ui_dirty = false;             /* painted; wait for next change */
    }
}

static void ui_action(void)
{
    ui_mark_dirty();                   /* an action may change visible state */
    switch (s_ui.page_id) {
    case UI_PAGE_PLAYER:
        if (s_pv == PV_SOURCE) {
            /* Enter the highlighted source: kick off a background load of that
             * directory, then switch to the track-list view. The order is fixed
             * by the filesystem + sort and cannot be reordered at runtime. */
            if (s_src_count == 0) {
                break;
            }
            player_load(PL_SRC_FOLDER, s_src_path[s_src_sel]);
            s_pv = PV_LIST;
            s_mp3_sel = 0;
            s_paint_mp3_sel = -1;   /* force list repaint */
            s_mp3_loading = true;   /* hide stale list until this load finishes */
            set_action("加载中");
            break;
        }
        if (s_mp3_count == 0) {
            set_action(player_scan_busy() ? "加载中..." : "无MP3文件");
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
            player_play(player_scan_path(s_mp3_sel));
        }
        break;
    case UI_PAGE_BT:
        if (bt_audio_is_connected()) {
            set_action("断开中");
            bt_audio_disconnect();
            /* Return output to the speaker immediately (the route is explicit;
             * we don't wait for the link to actually drop). */
            hw_audio_set_route(AUDIO_ROUTE_SPEAKER);
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
    case UI_PAGE_SETTINGS: {
        /* A press dispatches to the selected item's on_enter callback. Action
         * items (重建列表 / 重置NVS) do their work there; the 蓝牙 item opens
         * the Bluetooth management screen. Items with no on_enter are inert. */
        const setting_entry_t *e = &s_settings_table[s_setting_sel];
        if (e->on_enter) {
            e->on_enter();
        }
        break;
    }
    case UI_PAGE_EBOOK_LIST:
        if (ebook_scan_count() == 0) {
            set_action("无TXT文件");
            break;
        }
        if (!ebook_open(s_eb_sel)) {
            set_action("打开失败");
            break;
        }
        copy_book_name(s_eb_open_name, sizeof(s_eb_open_name),
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
        break;
    case UI_PAGE_PLAYER:
        if (s_pv == PV_SOURCE) {
            if (s_src_count > 0) {
                s_src_sel = (s_src_sel - step + s_src_count) % s_src_count;
            }
            break;
        }
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
        }
        break;
    case UI_PAGE_BT: {
        int count = bt_audio_device_count();
        if (count > 0) {
            s_bt_sel = (s_bt_sel - step + count) % count;
        }
        break;
    }
    case UI_PAGE_EBOOK_LIST: {
        int count = ebook_scan_count();
        if (count > 0) {
            s_eb_sel = (s_eb_sel - step + count) % count;
        }
        break;
    }
    default:
        return;
    }
    /* No synchronous ui_refresh(): the 60Hz main loop repaints from
     * s_ui_dirty, so rapid key repeats are never blocked by a redraw inside
     * the input callback. */
}

/* Left/right changes the value of the selected settings item (or flips the
 * ebook reading page). */
static void ui_adjust_lr(int dir)
{
    ui_mark_dirty();                   /* selected setting value changed */

    /* Music Player: left/right switch tracks. While a track is loaded, the
     * navigation is relative to the *playing* track (so you can keep skipping
     * forward/back from where you are); when idle it is relative to the
     * highlighted list row. Selection follows the new track.
     * - dir < 0: previous track (wraps to the last at the boundary)
     * - dir > 0: next track (wraps to the first at the boundary) */
    if (s_ui.page_id == UI_PAGE_PLAYER) {
        if (s_pv == PV_SOURCE) {
            return;   /* source picker: left/right are no-ops */
        }
        int count = player_scan_count();
        if (count == 0) {
            set_action(player_scan_busy() ? "扫描中..." : "无MP3文件");
        }
        else if (player_state() != PLAYER_IDLE) {
            /* Switch tracks while playing/paused: the player owns the index
             * math (wraps at the list ends, relative to the loaded track) and
             * starts the new track immediately. */
            int next = (dir < 0) ? player_prev() : player_next();
            s_mp3_sel = next;
            set_action(dir < 0 ? "上一首" : "下一首");
        }
        else {
            /* Idle: just move the cursor, like up/down does. */
            s_mp3_sel = (s_mp3_sel + dir + count) % count;
            set_action("选择");
        }
        ui_refresh();
        return;
    }

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
    const setting_entry_t *e = &s_settings_table[s_setting_sel];
    if (e->on_lr) {
        e->on_lr(dir);          /* callback owns dirty-marking + set_action */
        ui_refresh();
    }
}

static void ui_key_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_KEY) {
        return;
    }

    const uint32_t key = lv_event_get_key(e);

    /* While the screen is blanked in standby, this key press only wakes it
     * up: light the panel and re-arm the idle timer, but swallow the key so
     * the user does not accidentally trigger a selection/page change just by
     * wanting to "take a look". The next key press is acted upon normally. */
    if (hw_lcd_is_standby_active()) {
        hw_lcd_activity();
        return;
    }

    /* Any key press counts as user activity: re-arm the idle timer. */
    hw_lcd_activity();

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
    }
    /* Inside a detail page. */
    else if (key == LV_KEY_ESC) {
        ui_settings_flush();   /* commit any pending change before leaving */
        if (s_ui.page_id == UI_PAGE_PLAYER) {
            if (s_pv == PV_SOURCE) {
                /* On the source picker: B returns to the main menu. */
                ui_show_menu();
            }
            /* PV_LIST: B while playing/paused = stop but stay; while idle,
             * return up one level to the source picker (not the menu). */
            else if (player_state() != PLAYER_IDLE) {
                player_stop();
            }
            else {
                s_pv = PV_SOURCE;
                s_src_sel = 0;
                s_paint_src_sel = -1;   /* force source picker repaint */
                ui_mark_dirty();        /* ensure ui_refresh() actually repaints,
                                           otherwise the stale PV_LIST stays on
                                           screen until the next B press */
                ui_refresh();
            }
        }
        else if (s_ui.page_id == UI_PAGE_BT) {
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
    }
    else if (key == LV_KEY_HOME) {
        /* Select: switch the player's repeat mode (list loop ⇄ single-track
         * loop). Only meaningful on the player page; ignored elsewhere.
         * No toast — the mode is already shown in the top-right status
         * (">>单曲" / ">>列表"), so a bottom hint would be redundant. */
        if (s_ui.page_id == UI_PAGE_PLAYER) {
            ui_mark_dirty();   /* repaint the top-right status */
            player_repeat_toggle();
        }
    }
    else if (key == LV_KEY_ENTER) {
        ui_action();
    }
    else if (key == LV_KEY_UP) {
        ui_adjust(1);
    }
    else if (key == LV_KEY_DOWN) {
        ui_adjust(-1);
    }
    else if (key == LV_KEY_LEFT) {
        ui_adjust_lr(-1);
    }
    else if (key == LV_KEY_RIGHT) {
        ui_adjust_lr(1);
    }

    /* Instant feedback: repaint the state just changed and force a
     * synchronous render right here, so the press is visible on the panel in
     * this tick instead of waiting for the next LVGL refresh pass. Dirty
     * areas are small (a cursor row / a label), so the render + SPI flush
     * completes in a few ms and the UI feels immediate. Safe to call from an
     * event handler: lv_refr_now() pauses the refresh timer during the
     * synchronous render, and later invalidations (LV_EVENT_REFR_REQUEST)
     * resume the periodic 16 ms rendering. */
    ui_refresh();
    lv_refr_now(NULL);
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

    /* Persistent battery gauge in the top-right corner, above every page.
     * Layout: a 5-segment battery icon + "100%" text, anchored with absolute
     * screen coordinates (no clipping container), vertically centered with the
     * text. Style mirrors the reference look (cyan outline, 5 fill cells).
     * Parented to the ACTIVE SCREEN (sibling of the s_ui.screen container)
     * so it is drawn after the whole page/menu subtree and stays on top
     * regardless of when pages are (re)built. Pages must keep their content
     * clear of x >= 250 in the title row (status label is capped at width
     * 238 for this reason). */
    const lv_color_t bat_color = lv_color_hex(0x00FFFF); /* cyan */
    const int base_x = 250;
    const int base_y = 2;                       /* top of the percent text */
    const int icon_h = 12;                      /* battery body height */

    /* Percent label first, so we can center the icon against its height. */
    s_ui.bat_text = lv_label_create(lv_screen_active());
    lv_label_set_text(s_ui.bat_text, "--%");
    lv_obj_set_pos(s_ui.bat_text, base_x + 28, base_y);
    lv_obj_set_style_text_font(s_ui.bat_text, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(s_ui.bat_text, bat_color, 0);

    /* Vertically center the icon on the text's line height. */
    const int text_h = (int)lv_font_get_line_height(&lv_font_cn_16);
    const int bat_y = base_y + text_h / 2 - icon_h / 2;

    /* Body outline (24x12, 1px border), transparent interior. */
    lv_obj_t *body = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, 24, icon_h);
    lv_obj_set_pos(body, base_x, bat_y);
    lv_obj_set_style_border_color(body, bat_color, 0);
    lv_obj_set_style_border_width(body, 1, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    s_ui.battery = body;   /* existence flag */

    /* Positive terminal cap (2x5 px). */
    lv_obj_t *cap = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(cap);
    lv_obj_set_size(cap, 2, 5);
    lv_obj_set_pos(cap, base_x + 24, bat_y + (icon_h - 5) / 2);
    lv_obj_set_style_bg_color(cap, bat_color, 0);
    lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, 0);
    lv_obj_clear_flag(cap, LV_OBJ_FLAG_SCROLLABLE);
    s_ui.bat_cap = cap;

    /* 5 fill segments: 3 px wide, 6 px tall, 1 px gap; live inside the body. */
    for (int i = 0; i < 5; ++i) {
        lv_obj_t *seg = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(seg);
        lv_obj_set_size(seg, 3, 6);
        lv_obj_set_pos(seg, base_x + 3 + i * 4, bat_y + (icon_h - 6) / 2);
        lv_obj_set_style_bg_color(seg, bat_color, 0);
        lv_obj_set_style_bg_opa(seg, LV_OPA_TRANSP, 0);  /* hidden until updated */
        lv_obj_clear_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
        s_ui.bat_seg[i] = seg;
    }
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
    /* Poll the buttons faster than the default 16 ms LVGL refresh period so
     * a press (plus the 10 ms debounce) reaches the UI within ~15 ms instead
     * of ~40 ms. The read callback only scans 9 GPIOs, so the extra polls
     * are negligible. */
    lv_timer_set_period(lv_indev_get_read_timer(indev), BUTTON_POLL_PERIOD_MS);

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
