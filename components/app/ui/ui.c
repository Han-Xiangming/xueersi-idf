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
#include "bluetooth_audio.h"
#include "lcd.h"
#include "sd.h"
#include "ui.h"
#include "ui_internal.h"
#include "ui_nav.h"
#include "ui_strings.h"
#include "ui_theme.h"
#include "ui_widgets.h"
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
/* The single UI font (UI_FONT) and every colour / spacing / geometry token
 * come from ui_theme.h. */

/* Toast duration. (The debounced-settings write delay lives in
 * ui_internal.h next to the SETTINGS_DIRTY_* bits.) */
#define UI_ACTION_MSG_MS            850

static const char *const s_page_names[UI_PAGE_COUNT] = {
    "Music Player",
    "蓝牙",
    "设置",
    "电子书",
    "电子书",
};

/* Status-bar now-playing marquee (active while a track is loaded). */


/* Palette (UI_COLOR_*) and row geometry (UI_LIST_ROWS, UI_ROW_H_*,
 * UI_MENU_ROWS, ...) come from ui_theme.h — a restyle touches that file only. */


ui_state_t s_ui;
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

/* Cached async state so ui_refresh() can detect engine-side changes (Bluetooth
 * stack / player / SD) that arrive via callbacks or hardware rather than UI
 * input. */
static uint32_t        s_ext_bt_ver;
static int             s_ext_bt_count;
static bt_pair_state_t s_ext_bt_pair  = BT_PAIR_IDLE;
static bool            s_ext_bt_conn;
static bool            s_ext_bt_scan;
static uint8_t         s_ext_bt_retry;     /* last seen auto-retry count */
static bool            s_ext_sd_mounted;
static uint32_t        s_ext_scan_ver;    /* player's MP3 list refresh */
static player_state_t  s_ext_pl_state = PLAYER_IDLE;
static char            s_ext_pl_name[MP3_NAME_LEN];
static uint32_t        s_ext_eb_scan_ver;
static uint32_t        s_ext_eb_cnt_ver;
static uint8_t         s_ext_bat_pct = UINT8_MAX;   /* forces first paint */

void ui_mark_dirty(void)
{
    s_ui_dirty = true;
}

void copy_text(char *dst, size_t dst_size, const char *src)
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
void copy_utf8_clipped(char *dst, size_t dst_size, const char *src)
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
const char *strip_ext(const char *name)
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
int ui_text_px_width(const char *text)
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
bool ui_marquee_step(ui_marquee_t *mq, char *out, size_t out_size,
                            int max_w, const char *name)
{
    if (name == NULL) {
        name = "";
    }
    int w = ui_text_px_width(name);
    if (w <= max_w) {
        mq->scrolling = false;
        mq->ofs = 0;
        snprintf(out, out_size, "%s", name);
        return false;
    }
    /* Need to scroll. */
    if (mq->src[0] == '\0' || strncmp(mq->src, name, MQ_SRC_LEN) != 0) {
        /* New/changed name: (re)start from the beginning. */
        strncpy(mq->src, name, MQ_SRC_LEN - 1);
        mq->src[MQ_SRC_LEN - 1] = '\0';
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

void set_action(const char *msg)
{
    copy_text(s_action, sizeof(s_action), msg);
    /* The toast shows immediately on the next refresh and stays up for
     * UI_ACTION_MSG_MS. Rapid repeats (key auto-repeat) only update the text
     * and extend the expiry. */
    s_action_until_ms = lv_tick_get() + UI_ACTION_MSG_MS;
    s_ui_dirty = true;                 /* refresh until the toast expires */
}

/* ui_theme_text_set() (set a label only when the text really changed) lives
 * in ui_theme.c. */

/* Step the master volume by `dir` (+1/-1): 1% steps while at/below 10%,
 * 5% steps above 10% — same scheme as the backlight control. */
void ui_volume_step(int dir)
{
    int v = (int)hw_audio_get_volume();
    int step = (v <= 10) ? 1 : 5;
    v += dir * step;
    hw_audio_set_volume((uint8_t)MAX(0, MIN(v, 100)));
}

/* Label / page / bar constructors live in ui_theme.c. */

void ui_set_hint(const char *normal)
{
    if (!s_ui.hint) {
        return;
    }
    const uint32_t now_ms = lv_tick_get();
    if (s_action_until_ms && (int32_t)(s_action_until_ms - now_ms) > 0) {
        ui_theme_text_set(s_ui.hint, s_action);
        /* Toast takes over the status row: hide the ebook bar/percentage
         * so the centered toast text does not collide with them. */
        if (s_ui.eb_bar && s_ui.eb_pct) {
            lv_obj_add_flag(s_ui.eb_bar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_ui.eb_pct, LV_OBJ_FLAG_HIDDEN);
        }
    }
    else {
        s_action_until_ms = 0;         /* toast expired: arm the next one afresh */
        ui_theme_text_set(s_ui.hint, normal);
        if (s_ui.eb_bar && s_ui.eb_pct) {
            lv_obj_remove_flag(s_ui.eb_bar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_ui.eb_pct, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Settings table callbacks                                           */
/* ------------------------------------------------------------------ */
/* Forward declarations (defined further below).
 *  - ui_enter_page(): rebuild and show a page. It does NOT touch the nav
 *    stack, so it is equally usable for going forward and for going back.
 *  - ui_go(): the forward move — record the page on the stack, then show it.
 */
void ui_enter_page(ui_page_t page);
void ui_go(ui_page_t page);


static void ui_build_page_content(lv_obj_t *page)
{
    /* Header row sits 2 px left of the default margin so the whole top bar
     * reads as one unit, clear of the battery overlay. */
    s_ui.title = ui_theme_label_at(page, s_page_names[ui_nav_current()],
                                   UI_TITLE_X, UI_TITLE_Y, UI_TEXT_W,
                                   UI_COLOR_TITLE, LV_TEXT_ALIGN_LEFT);
    /* Top-right status label. Secondary pages leave it empty; the Player page
     * fills it with the playback state (>> / || / --) in ui_refresh(). The
     * top-right corner (x >= UI_BATTERY_ZONE_X) is claimed by the persistent
     * battery gauge drawn above the page container, so the status text must
     * end before it. */
    s_ui.status = ui_theme_label(page, "", UI_TITLE_Y, UI_COLOR_TEXT_DIM,
                                 LV_TEXT_ALIGN_RIGHT);
    lv_obj_set_width(s_ui.status, UI_STATUS_W);

    /* Header separator, matching the main-menu style. */
    ui_theme_separator(page, UI_SEP_Y);

    if (ui_nav_current() == UI_PAGE_SETTINGS) {
        ui_build_settings(page);
        return;
    }
    if (ui_nav_current() == UI_PAGE_PLAYER) {
        ui_build_player(page);
        return;
    }
    if (ui_nav_current() == UI_PAGE_BT) {
        ui_build_bt(page);
        return;
    }
    if (ui_nav_current() == UI_PAGE_EBOOK_LIST) {
        ui_build_ebook_list(page);
        return;
    }
    if (ui_nav_current() == UI_PAGE_EBOOK_READ) {
        /* Clip the book title before it collides with the page-number
         * status label in the top-right corner. */
        lv_obj_set_width(s_ui.title, 200);
        ui_build_ebook_read(page);
        return;
    }

    /* Generic value/bar page (used by the SD CARD page). */
    s_ui.value = ui_theme_label(page, "--", 50, UI_COLOR_ACCENT, LV_TEXT_ALIGN_CENTER);
    s_ui.sub = ui_theme_label(page, "--", 88, UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_CENTER);
    s_ui.bar = ui_theme_bar(page, 0);
    s_ui.hint = ui_theme_label(page, "A重扫 B返回", UI_LEGEND_Y, UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_CENTER);
}

void ui_show_menu(void)
{
    ui_nav_home();
    ui_launcher_reset();
    if (s_ui.page) {
        lv_obj_add_flag(s_ui.page, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(s_ui.menu_page, LV_OBJ_FLAG_HIDDEN);
    ui_refresh_launcher();
}

/*
 * Show a page: destroy the old container and rebuild this one.
 * The navigation stack is deliberately left alone — ui_go() pushes before
 * calling this, ui_nav_back_or_menu() pops before calling it. Mixing the two
 * would double-push on the way back and trap the user on the child page.
 */
void ui_enter_page(ui_page_t page)
{
    lv_obj_add_flag(s_ui.menu_page, LV_OBJ_FLAG_HIDDEN);
    if (s_ui.page) {
        lv_obj_delete(s_ui.page);
        s_ui.page = NULL;
    }
    /* Per-page entry work (cache re-query, playlist cache load, bringing the
     * Bluetooth stack up) now lives in each page's build function — this layer
     * only assembles. */
    s_ui.page = ui_theme_page(s_ui.screen);
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
    ui_player_reset_paint();
    ui_bt_reset_paint();
    ui_ebook_reset_paint();
    ui_settings_reset_paint();
    ui_mark_dirty();
    ui_refresh();
}

/*
 * Step out of the current page: pop the navigation stack and rebuild whatever
 * the user was on before, or fall back to the main menu once the stack runs
 * out. Pages no longer hard-code their own back target (BT -> Settings,
 * reader -> book list) — "where did I come from" is recorded in the stack.
 */
void ui_nav_back_or_menu(void)
{
    ui_page_t parent;
    if (ui_nav_pop(&parent)) {
        ui_enter_page(parent);
    }
    else {
        ui_show_menu();
    }
}

/* Forward navigation: remember where we came from, then show the page. */
void ui_go(ui_page_t page)
{
    ui_nav_push(page);
    ui_enter_page(page);
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
    uint32_t v   = bluetooth_audio_device_version();
    int cnt     = bluetooth_audio_device_count();
    bt_pair_state_t ps = bluetooth_audio_pair_state();
    bool conn    = bluetooth_audio_is_connected();
    bool scan    = bluetooth_audio_is_scanning();
    if (v != s_ext_bt_ver || cnt != s_ext_bt_count || ps != s_ext_bt_pair
        || conn != s_ext_bt_conn || scan != s_ext_bt_scan
        || bluetooth_audio_retry_count() != s_ext_bt_retry) {
        s_ext_bt_ver   = v;
        s_ext_bt_count = cnt;
        s_ext_bt_pair  = ps;
        s_ext_bt_conn  = conn;
        s_ext_bt_scan  = scan;
        s_ext_bt_retry = bluetooth_audio_retry_count();
        changed = true;
    }

    /* SD mount can change without UI input (card removed / inserted). */
    bool mnt = hw_sd_is_mounted();
    if (mnt != s_ext_sd_mounted) {
        s_ext_sd_mounted = mnt;
        player_notify_sd_remount();   /* card changed: invalidate old snapshot */
        player_scan_with_cache();   /* prefer cache; scan only if absent */
        /* Let the pages re-discover their sources / reload the current list.
         * Must run after player_scan_with_cache() so the "scan in flight"
         * guard sees the real state. */
        ui_player_notify_sd_change();
        ui_ebook_notify_sd_change();
        changed = true;
    }

    /* Background MP3 list scan completed: repaint the player list. */
    uint32_t sv = player_scan_version();
    if (sv != s_ext_scan_ver) {
        s_ext_scan_ver = sv;
        changed = true;
    }

    /* Player state / track affects the PLAYER page and the global mini
     * player bar, so watch it on every page. */
    player_state_t st = player_state();
    const char *nm    = player_current_name();
    if (st != s_ext_pl_state
        || strncmp(nm, s_ext_pl_name, MP3_NAME_LEN - 1) != 0) {
        s_ext_pl_state = st;
        strncpy(s_ext_pl_name, nm, MP3_NAME_LEN - 1);
        s_ext_pl_name[MP3_NAME_LEN - 1] = '\0';
        changed = true;
    }

    /* Ebook: the scan list and the background page count land asynchronously. */
    if (ui_nav_current() == UI_PAGE_EBOOK_LIST) {
        uint32_t sv = ebook_scan_version();
        if (sv != s_ext_eb_scan_ver) {
            s_ext_eb_scan_ver = sv;
            changed = true;
        }
    }
    else if (ui_nav_current() == UI_PAGE_EBOOK_READ) {
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
    0xF85149,          /* red    - critical (<=15%)  */
    UI_COLOR_WARN,     /* orange - low      (<=35%)  */
    0xFFE000,          /* yellow - medium   (<=60%)  */
    UI_COLOR_ACCENT,   /* cyan   - good      (<=85%) */
    UI_COLOR_OK,       /* green  - full      (<=100%) */
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
}


void ui_refresh(void)
{
    ui_settings_flush();
    if (ui_nav_in_menu()) {
        /* Menu is event-driven, but keep the battery gauge and the playback
         * panel live on every tick. */
        ui_refresh_battery();
        ui_player_refresh_panel();
        return;                         /* menu is event-driven */
    }

    /* An action toast is still counting down (s_action_until_ms set): keep
     * refreshing until ui_set_hint() clears it on expiry, even if nothing else
     * changed. */
    bool toast_active = (s_action_until_ms != 0);

    /* Marquees are advanced every tick even when the page is otherwise idle:
     * only the scrolling label is touched, so the main loop stays free and
     * every key stays responsive (a full repaint per frame starves the button
     * poller). */
    if (ui_nav_current() == UI_PAGE_PLAYER) {
        ui_player_prog_marquee();       /* now-playing status bar */
        ui_player_list_marquee();       /* selected track row */
    }
    else if (ui_nav_current() == UI_PAGE_EBOOK_LIST) {
        ui_ebook_list_marquee();        /* selected book row */
    }

    if (ui_external_changed()) {
        s_ui_dirty = true;             /* Bluetooth/player/SD changed via callback */
    }
    if (!s_ui_dirty && !toast_active) {
        return;                         /* idle: nothing visible changed */
    }

    /* Guards against recomputing the page before its labels are built. Leave
     * s_ui_dirty set so we retry on the next tick. */
    if (ui_nav_current() == UI_PAGE_SETTINGS) {
        if (!s_ui.set_cursor[0] || !s_ui.hint) {
            return;
        }
    }
    else if (ui_nav_current() == UI_PAGE_PLAYER) {
        if (!s_ui.pl_list.cursor[0] || !s_ui.hint) {
            return;
        }
    }
    else if (ui_nav_current() == UI_PAGE_BT) {
        if (!s_ui.bt_list.cursor[0] || !s_ui.hint) {
            return;
        }
    }
    else if (ui_nav_current() == UI_PAGE_EBOOK_LIST) {
        if (!s_ui.eb_list.cursor[0] || !s_ui.hint) {
            return;
        }
    }
    else if (ui_nav_current() == UI_PAGE_EBOOK_READ) {
        if (!s_ui.eb_text_label || !s_ui.hint) {
            return;
        }
    }
    else if (!s_ui.value || !s_ui.sub || !s_ui.hint) {
        return;
    }

    /* Per-page paint. Each page owns its rows and its live state. */
    switch (ui_nav_current()) {
    case UI_PAGE_SETTINGS:   ui_refresh_settings();   break;
    case UI_PAGE_PLAYER:     ui_refresh_player();     break;
    case UI_PAGE_BT:         ui_refresh_bt();         break;
    case UI_PAGE_EBOOK_LIST: ui_refresh_ebook_list(); break;
    case UI_PAGE_EBOOK_READ: ui_refresh_ebook_read(); break;
    default: break;
    }
    ui_refresh_battery();
    ui_player_refresh_panel();
    /* The marquees above never force a repaint: they are advanced every tick
     * via pinpoint label updates (see ui_refresh()). So once painted we simply
     * clear the dirty flag and let the main loop go idle, which keeps every key
     * responsive while a long filename scrolls. */
    s_ui_dirty = false;                 /* painted; wait for next change */
}

static void ui_action(void)
{
    ui_mark_dirty();                   /* an action may change visible state */
    switch (ui_nav_current()) {
    case UI_PAGE_PLAYER:     ui_player_action();      break;
    case UI_PAGE_BT:         ui_bt_action();          break;
    case UI_PAGE_SETTINGS:   ui_settings_action();    break;
    case UI_PAGE_EBOOK_LIST: ui_ebook_list_action();  break;
    case UI_PAGE_EBOOK_READ: ui_ebook_read_action();  break;
    default: break;
    }
    ui_refresh();
}

static void ui_adjust(int step)
{
    ui_mark_dirty();                   /* selection / scroll changed */
    switch (ui_nav_current()) {
    case UI_PAGE_SETTINGS:   ui_settings_adjust(step); break;
    case UI_PAGE_PLAYER:     ui_player_adjust(step);   break;
    case UI_PAGE_BT:         ui_bt_adjust(step);       break;
    case UI_PAGE_EBOOK_LIST: ui_ebook_adjust(step);    break;
    default: break;
    }
    /* No synchronous ui_refresh(): the 60Hz main loop repaints from
     * s_ui_dirty, so rapid key repeats are never blocked by a redraw inside
     * the input callback. */
}

/* Left/right: adjust the selected settings item, switch tracks on the player,
 * or flip the reader page. */
static void ui_adjust_lr(int dir)
{
    ui_mark_dirty();                   /* selected setting value changed */
    switch (ui_nav_current()) {
    case UI_PAGE_PLAYER:     ui_player_lr(dir);   break;
    case UI_PAGE_EBOOK_READ: ui_ebook_lr(dir);    break;
    case UI_PAGE_SETTINGS:   ui_settings_lr(dir); break;
    default: break;                    /* no left/right meaning on other pages */
    }
}

static void ui_key_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_KEY) {
        return;
    }

    const uint32_t key = lv_event_get_key(e);

    /* Global media key (MENU = toggle the floating playback panel), active on
     * every page and even while the panel is blanked (it wakes the screen
     * too). The buttons driver delivers it single-shot (no auto-repeat),
     * so one press is exactly one action. START is deliberately unbound: it
     * only wakes the screen via the standby handler below. */
    if (key == LV_KEY_MEDIA_PANEL) {
        hw_lcd_activity();
        /* On the player page MENU returns to the main menu (the playback
         * panel is ebook-only and cannot be open here). */
        if (!ui_nav_in_menu() && ui_nav_current() == UI_PAGE_PLAYER) {
            ui_show_menu();
            ui_refresh();
            lv_refr_now(NULL);
            return;
        }
        const bool on_ebook = !ui_nav_in_menu()
                              && (ui_nav_current() == UI_PAGE_EBOOK_LIST
                                  || ui_nav_current() == UI_PAGE_EBOOK_READ);
        /* The floating panel may only be summoned on the ebook pages (it is
         * the background-music companion while reading). A closed panel
         * ignores the key anywhere else; an open one always closes. */
        if (ui_player_panel_open() || on_ebook) {
            ui_player_panel_toggle();
        }
        ui_refresh();
        lv_refr_now(NULL);
        return;
    }

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

    /* The playback panel is modal: while it is open it owns all keys. */
    if (ui_player_panel_key(key)) {
        ui_refresh();
        lv_refr_now(NULL);
        return;
    }

    if (ui_nav_in_menu()) {
        /* The launcher is a 3×2 icon grid; arrows walk it, ENTER opens the
         * selected page. B (ESC) is a no-op while already on the home screen. */
        if (key == LV_KEY_UP || key == LV_KEY_DOWN ||
            key == LV_KEY_LEFT || key == LV_KEY_RIGHT) {
            ui_launcher_nav(key);
        }
        else if (key == LV_KEY_ENTER) {
            ui_go(ui_launcher_page());
        }
    }
    /* Inside a detail page. */
    else if (key == LV_KEY_ESC) {
        ui_settings_flush();   /* commit any pending change before leaving */
        /* A page may handle B itself (step out of an inner view, stop
         * playback, cancel the jump overlay...). Only when it does not do we
         * walk the navigation stack back. */
        switch (ui_nav_current()) {
        case UI_PAGE_PLAYER:
            if (!ui_player_esc()) {
                ui_nav_back_or_menu();
            }
            break;
        case UI_PAGE_EBOOK_LIST:
        case UI_PAGE_EBOOK_READ:
            if (!ui_ebook_esc()) {
                ui_nav_back_or_menu();
            }
            break;
        case UI_PAGE_SETTINGS:
            /* Inside a group's item list, B steps back to the group list; on the
             * group list itself, B pops to the launcher. */
            if (!ui_settings_esc()) {
                ui_nav_back_or_menu();
            }
            break;
        default:
            /* Bluetooth and any future page. */
            ui_nav_back_or_menu();
            break;
        }
    }
    else if (key == LV_KEY_HOME) {
        /* Select is page-specific: cycle the repeat mode on the player, open
         * the jump overlay in the reader, re-scan on Bluetooth. */
        switch (ui_nav_current()) {
        case UI_PAGE_PLAYER:     ui_player_select(); break;
        case UI_PAGE_EBOOK_READ: ui_ebook_select();  break;
        case UI_PAGE_BT:         ui_bt_select();     break;
        default: break;
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
    ui_nav_init();          /* boot on the home screen: empty page stack */

    s_ui.screen = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_ui.screen);
    lv_obj_set_size(s_ui.screen, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_style_bg_color(s_ui.screen, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_ui.screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_ui.screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ui.screen, LV_OBJ_FLAG_CLICKABLE);
    lv_group_add_obj(group, s_ui.screen);
    lv_group_focus_obj(s_ui.screen);
    lv_obj_add_event_cb(s_ui.screen, ui_key_event_cb, LV_EVENT_KEY, NULL);
    /* Use the embedded CJK font everywhere so Chinese text renders. */
    lv_obj_set_style_text_font(s_ui.screen, UI_FONT, 0);

    ui_build_launcher();
    ui_refresh_launcher();

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
    lv_obj_set_style_text_font(s_ui.bat_text, UI_FONT, 0);
    lv_obj_set_style_text_color(s_ui.bat_text, bat_color, 0);

    /* Vertically center the icon on the text's line height. */
    const int text_h = (int)lv_font_get_line_height(UI_FONT);
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

    /* Floating playback control panel (MENU key): built once, hidden by
     * default; parented to the ACTIVE SCREEN like the battery gauge so it
     * draws above every page/menu and survives page rebuilds. */
    ui_player_build_panel();
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
    /* Tuned repeat: 250 ms to the first auto-repeat (was 360 ms — a hold
     * felt like it stalled), then every 90 ms. Matches typical handheld
     * keypad feel for list scrolling and volume ramping. */
    lv_indev_set_long_press_time(indev, 250);
    lv_indev_set_long_press_repeat_time(indev, 90);
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
