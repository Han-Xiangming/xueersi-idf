/*
 * Music player page: source picker, track list, floating control panel
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
 * source — <ALL> (whole /sdcard/Music tree) or one sub-folder directly under
 * /sdcard/Music. The chosen source determines which directory player_load()
 * scans. The player layer is source-agnostic (it scans any root; the default
 * boot scan is the whole card via PLAYER_ROOT), but this picker only exposes
 * the /sdcard/Music subtree for user convenience. The track ORDER within a
 * source is fixed (sorted name) and cannot be changed at runtime — there is
 * deliberately no reorder UI; only a fresh load may change it (by rescanning
 * the filesystem). */
typedef enum {
    PV_SOURCE = 0,   /* picking a source: <ALL> + folders */
    PV_LIST,         /* browsing/playing the loaded playlist */
} player_view_t;

#define SRC_MAX 128
/* UI-only source list data (~72 KB): keep it in external PSRAM (EXT_RAM_BSS)
 * so it does not compete with the Bluetooth stack for internal DRAM. */
EXT_RAM_BSS_ATTR static char s_src_list[SRC_MAX][MP3_NAME_LEN];  /* folder names; [0]="" => <ALL> */
EXT_RAM_BSS_ATTR static char s_src_path[SRC_MAX][PLAYER_PATH_LEN];/* absolute dir path per entry */
static int      s_src_count;
static int      s_src_sel;
static player_view_t s_pv = PV_SOURCE;
static int      s_paint_src_sel = -1;
static ui_marquee_t s_mp3_mq;
static ui_marquee_t s_prog_mq;
/* True once pl_prog has been switched to left-align for scrolling, so we don't
 * re-set the style every refresh. */
static bool s_prog_align_left = false;
static int s_paint_mp3_sel  = -1;
static int s_paint_mp3_top  = -1;
static void ui_pl_prog(const char *text, bool left)
{
    if (s_prog_align_left != left) {
        lv_obj_set_style_text_align(s_ui.pl_prog,
            left ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);
        s_prog_align_left = left;
    }
    if (!left) {
        s_prog_mq.scrolling = false;
    }
    ui_theme_text_set(s_ui.pl_prog, text);
}

/* UI source-picker root: the user picks a music source from the FOLDERS
 * directly under /sdcard/Music. Each sub-folder is one selectable source;
 * <ALL> (entry 0) scans the whole /sdcard/Music tree. This is a UI-level
 * convenience rooted here; the player layer itself scans any root it is given
 * (see PLAYER_ROOT in player.h — its default boot scan is the whole card). */
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

void ui_build_player(lv_obj_t *page)
{
    s_mp3_sel = 0;
    s_src_sel = 0;
    s_pv = PV_SOURCE;
    /* Prefer the on-card playlist cache so the list shows instantly; only fall
     * back to a real scan (which rewrites the cache) when no cache exists —
     * never blocks the UI on the FATFS walk. */
    player_scan_with_cache();
    /* If the cache was missing, a background scan is now in flight. Suppress
     * any stale list in the track view until it publishes, so we don't flash
     * the previous folder's contents. (The source picker is unaffected;
     * s_mp3_loading only gates the PV_LIST rows.) */
    if (player_scan_busy()) {
        s_mp3_loading = true;
    }
    ui_discover_sources();

    /* 6 shared list rows: the source picker and the track list reuse them. */
    ui_list_create(&s_ui.pl_list, page, UI_LIST_ROWS, UI_LIST_FIRST_Y, UI_ROW_H_LIST);

    s_ui.pl_prog = ui_theme_label(page, UI_STR_SEL_MUSIC, 196, UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_CENTER);
    s_ui.hint = ui_theme_label(page, UI_STR_HINT_NAV, UI_LEGEND_Y, UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_CENTER);
}

/* ------------------------------------------------------------------ */
/* Floating playback control panel (MENU key)                          */
/* ------------------------------------------------------------------ */

/* Panel controls: prev / play-pause / next / stop / repeat-mode toggle. */
#define PLAYER_PANEL_NBTN 5

/* Width of a label in px: every glyph in lv_font_cn_16 is 16 px wide and
 * every label here is pure CJK, so counting UTF-8 characters (skipping the
 * 0b10xxxxxx continuation bytes) gives the exact width without font metrics. */
static int ui_label_width_px(const char *s)
{
    int n = 0;
    while (*s != '\0') {
        if ((*s & 0xC0) != 0x80) {
            n++;
        }
        s++;
    }
    return n * 16;
}

/* Short name of the current repeat mode for the status texts and the
 * floating-panel 循环 button: UI_STR_REPEAT_ONE / UI_STR_REPEAT_LIST / UI_STR_REPEAT_RANDOM. */
static const char *ui_repeat_text(void)
{
    switch (player_repeat_mode()) {
    case PLAYER_REPEAT_ONE:    return UI_STR_REPEAT_ONE;
    case PLAYER_REPEAT_RANDOM: return UI_STR_REPEAT_RANDOM;
    default:                   return UI_STR_REPEAT_LIST;
    }
}

static bool s_panel_open;
static int  s_panel_sel;   /* 0..4, index into the five controls */

static const char *const s_panel_labels[PLAYER_PANEL_NBTN] = {
    UI_STR_PREV, UI_STR_PLAY, UI_STR_NEXT, UI_STR_STOP, UI_STR_LOOP,
};
/* The play/pause button label is state-dependent (player-page convention:
 * UI_STR_PAUSE while playing, UI_STR_RESUME while paused); the repeat button shows the
 * current mode (UI_STR_REPEAT_ONE/UI_STR_REPEAT_LIST/UI_STR_REPEAT_RANDOM) so pressing it reads as "cycle to the
 * next mode". */
static const char *ui_panel_btn_text(int i)
{
    if (i == 1) {
        switch (player_state()) {
        case PLAYER_PLAYING: return UI_STR_PAUSE;
        case PLAYER_PAUSED:  return UI_STR_RESUME;
        default:             return UI_STR_PLAY;
        }
    }
    if (i == 4) {
        return ui_repeat_text();
    }
    return s_panel_labels[i];
}

/* Build the floating panel once at startup (sibling of the battery gauge on
 * the ACTIVE SCREEN, so it draws above every page/menu). Hidden by default;
 * MENU toggles it, B or MENU closes it. Layout (screen 320x240):
 *   panel (12,64,296,136), 1px border, near-black fill
 *   row 1: track name (left) + ">>列表"/"||单曲"/"--随机" (right)
 *   row 2: five controls, laid out from the exact label widths with a
 *          uniform 12 px gap, centered in the panel
 *   row 3: hint UI_STR_HINT_PANEL */
void ui_player_build_panel(void)
{
    lv_obj_t *panel = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(panel);
    lv_obj_set_pos(panel, 12, 64);
    lv_obj_set_size(panel, 296, 136);
    lv_obj_set_style_bg_color(panel, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(UI_COLOR_LINE), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
    s_ui.pl_panel = panel;

    s_ui.pl_panel_name = lv_label_create(panel);
    lv_label_set_text(s_ui.pl_panel_name, "");
    lv_label_set_long_mode(s_ui.pl_panel_name, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_pos(s_ui.pl_panel_name, 10, 8);
    lv_obj_set_size(s_ui.pl_panel_name, 190, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(s_ui.pl_panel_name, UI_FONT, 0);
    lv_obj_set_style_text_color(s_ui.pl_panel_name, lv_color_hex(UI_COLOR_TEXT), 0);

    s_ui.pl_panel_state = lv_label_create(panel);
    lv_label_set_text(s_ui.pl_panel_state, "");
    lv_label_set_long_mode(s_ui.pl_panel_state, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_pos(s_ui.pl_panel_state, 208, 8);
    lv_obj_set_size(s_ui.pl_panel_state, 78, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(s_ui.pl_panel_state, UI_FONT, 0);
    lv_obj_set_style_text_color(s_ui.pl_panel_state, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_align(s_ui.pl_panel_state, LV_TEXT_ALIGN_RIGHT, 0);

    /* Evenly distribute the five controls: fixed-width slots left 4 px /
     * 20 px alternating gaps (3-char vs 2-char labels); laying the row out
     * from the exact label widths with a uniform gap and centered margins
     * makes every inter-button gap identical whatever the labels read. */
    int s_panel_x[PLAYER_PANEL_NBTN];
    {
        const int gap = 12;
        /* Panel width from the literal set above (296): lv_obj_get_width()
         * would return the laid-out coords box, which is still empty at
         * build time (the panel is created before the first layout pass),
         * pushing the whole row off the panel's left edge. */
        const int pw = 296;
        int total = 0;
        for (int i = 0; i < PLAYER_PANEL_NBTN; i++) {
            total += ui_label_width_px(s_panel_labels[i]);
        }
        int x = (pw - total - gap * (PLAYER_PANEL_NBTN - 1)) / 2;
        for (int i = 0; i < PLAYER_PANEL_NBTN; i++) {
            s_panel_x[i] = x;
            x += ui_label_width_px(s_panel_labels[i]) + gap;
        }
    }
    for (int i = 0; i < PLAYER_PANEL_NBTN; i++) {
        lv_obj_t *btn = lv_label_create(panel);
        lv_label_set_text(btn, ui_panel_btn_text(i));
        lv_label_set_long_mode(btn, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_pos(btn, s_panel_x[i], 72);
        lv_obj_set_size(btn, ui_label_width_px(s_panel_labels[i]),
                        LV_SIZE_CONTENT);
        lv_obj_set_style_text_font(btn, UI_FONT, 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
        s_ui.pl_panel_btn[i] = btn;
    }

    s_panel_open = false;

    lv_obj_t *hint = lv_label_create(panel);
    lv_label_set_text(hint, UI_STR_HINT_PANEL);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_pos(hint, 10, 104);
    /* Fixed height instead of LV_SIZE_CONTENT: the size would be computed
     * with the default (montserrat_10) font at creation time and the hint
     * text never changes, so CLIP would cut the 15px CJK glyphs top and
     * bottom. 30px = lv_font_cn_16 line height, keeps the whole row. */
    lv_obj_set_size(hint, 276, 30);
    lv_obj_set_style_text_font(hint, UI_FONT, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
}

/* Repaint the panel's live content: track name + state symbol, the
 * state-dependent play/pause button label, and the selection highlight. */
void ui_player_refresh_panel(void)
{
    if (s_ui.pl_panel == NULL || !s_panel_open) {
        return;
    }
    const player_state_t st = player_state();

    static char s_panel_name_buf[MP3_NAME_LEN + 8];
    if (st == PLAYER_IDLE) {
        snprintf(s_panel_name_buf, sizeof(s_panel_name_buf), UI_STR_NOT_PLAYING);
    }
    else {
        snprintf(s_panel_name_buf, sizeof(s_panel_name_buf), "%s",
                 strip_ext(player_current_name()));
    }
    ui_theme_text_set(s_ui.pl_panel_name, s_panel_name_buf);

    static char s_panel_state_buf[16];
    snprintf(s_panel_state_buf, sizeof(s_panel_state_buf), "%s%s",
             st == PLAYER_PLAYING ? ">>" : st == PLAYER_PAUSED ? "||" : "--",
             ui_repeat_text());
    ui_theme_text_set(s_ui.pl_panel_state, s_panel_state_buf);

    for (int i = 0; i < PLAYER_PANEL_NBTN; i++) {
        ui_theme_text_set(s_ui.pl_panel_btn[i], ui_panel_btn_text(i));
        lv_obj_set_style_text_color(s_ui.pl_panel_btn[i],
                                    lv_color_hex(i == s_panel_sel ? UI_COLOR_ACCENT : UI_COLOR_TEXT_DIM), 0);
    }
}

/* A on the panel: run the selected control. */
static void ui_panel_activate(void)
{
    switch (s_panel_sel) {
    case 0:   /* 上一曲 */
        if (player_state() != PLAYER_IDLE) {
            player_prev();
            set_action(UI_STR_PREV);
        }
        else {
            set_action(UI_STR_NOT_PLAYING);
        }
        break;
    case 1:   /* 播放 / 暂停 / 继续 */
        if (player_state() != PLAYER_IDLE) {
            player_toggle();
            set_action(player_state() == PLAYER_PLAYING ? UI_STR_PLAYING : UI_STR_PAUSED);
        }
        else {
            set_action(UI_STR_NOT_PLAYING);
        }
        break;
    case 2:   /* 下一曲 */
        if (player_state() != PLAYER_IDLE) {
            player_next();
            set_action(UI_STR_NEXT);
        }
        else {
            set_action(UI_STR_NOT_PLAYING);
        }
        break;
    case 4:   /* 循环: 切换播放模式 (列表循环 -> 单曲循环 -> 随机 -> 列表...).
               * The button label shows the CURRENT mode; pressing cycles to
               * the next one. */
        player_repeat_toggle();
        set_action(player_repeat_mode() == PLAYER_REPEAT_ONE
                       ? UI_STR_LOOP_ONE
                       : player_repeat_mode() == PLAYER_REPEAT_RANDOM
                             ? UI_STR_LOOP_RANDOM : UI_STR_LOOP_LIST);
        break;
    default:  /* 停止 */
        if (player_state() != PLAYER_IDLE) {
            player_stop();
            set_action(UI_STR_STOPPED);
        }
        break;
    }
    ui_mark_dirty();
}
/* Now-playing status-bar marquee: advanced every tick by the assembler. */
void ui_player_prog_marquee(void)
{
    /* Driven every UI tick (even when the page is not dirty) so the
     * now-playing line keeps scrolling without forcing a full-page repaint.
     * A full repaint every frame would keep the main loop busy and starve
     * the button poller, making the B key require a long press — the
     * regression introduced with the status-bar marquee. Here we only touch
     * pl_prog, so the rest of the page stays idle. */
    if (ui_nav_in_menu() || ui_nav_current() != UI_PAGE_PLAYER || !s_ui.pl_prog) {
        return;
    }
    if (player_state() == PLAYER_IDLE) {
        return;                         /* idle text is set by ui_paint */
    }
    const char *src = player_current_src_name();
    char prefix[MP3_NAME_LEN + 4];
    snprintf(prefix, sizeof(prefix), "[%s] ", src);
    int track_max = PROG_LINE_W - ui_text_px_width(prefix);
    if (track_max < 8) {
        track_max = 8;                  /* guard a very long source name */
    }
    const char *track = strip_ext(player_current_name());
    char track_buf[MQ_SRC_LEN];
    ui_marquee_step(&s_prog_mq, track_buf, sizeof(track_buf), track_max, track);
    char prog[MQ_SRC_LEN + MP3_NAME_LEN + 4];
    snprintf(prog, sizeof(prog), "%s%s", prefix, track_buf);
    ui_pl_prog(prog, s_prog_mq.scrolling);
}

/* Selected track row marquee (long filename scrolls). */
void ui_player_list_marquee(void)
{
    if (s_pv != PV_LIST || player_state() == PLAYER_PLAYING) {
        return;
    }
    if (!s_mp3_mq.scrolling) {
        return;                     /* selected name fits / no scroll */
    }
    int top = ui_list_top(s_mp3_sel, s_mp3_count, UI_LIST_ROWS);
    int i = s_mp3_sel - top;
    if (i < 0 || i >= UI_LIST_ROWS) {
        return;                     /* selected row off-screen */
    }
    static char out[MP3_NAME_LEN];
    ui_marquee_step(&s_mp3_mq, out, sizeof(out), LIST_LINE_W,
                    strip_ext(player_scan_name(s_mp3_sel)));
    ui_theme_text_set(s_ui.pl_list.text[i], out);
}

void ui_refresh_player(void)
{
        if (s_pv == PV_SOURCE) {
            /* Source picker: <ALL> (whole card) + top-level folders. */
            int top = ui_list_top(s_src_sel, s_src_count, UI_LIST_ROWS);
            const bool sel_changed = (s_src_sel != s_paint_src_sel)
                                   || (top != s_paint_mp3_top);
            for (int i = 0; i < UI_LIST_ROWS; i++) {
                int idx = top + i;
                const int sel = (idx == s_src_sel);
                if (idx < s_src_count) {
                    static char s_src_buf[MP3_NAME_LEN];
                    const char *label = (s_src_list[idx][0] == '\0')
                                      ? "<ALL>" : s_src_list[idx];
                    copy_utf8_clipped(s_src_buf, sizeof(s_src_buf), label);
                    ui_list_row(&s_ui.pl_list, i, idx, s_src_count, s_src_buf, sel, false);
                }
                else {
                    ui_list_row(&s_ui.pl_list, i, idx, s_src_count, "", false, true);
                }
            }
            if (sel_changed) {
                s_paint_src_sel = s_src_sel;
                s_paint_mp3_top = top;
            }
            ui_theme_text_set(s_ui.status, "--");
            ui_pl_prog(UI_STR_SEL_MUSIC, false);
            ui_set_hint(UI_STR_HINT_NAV);
            return;
        }

        /* PV_LIST: the track list (published by the background load). Re-fetch
         * each pass so a completed load shows up without a page rebuild. */
        s_mp3_count = player_scan_count();
        if (s_mp3_sel >= s_mp3_count) {
            s_mp3_sel = s_mp3_count > 0 ? s_mp3_count - 1 : 0;
        }
        /* While a (sub)folder load is in flight, s_playlist still holds the
         * previous folder's contents. Suppress drawing it so we don't flash
         * the old list for a frame before the scan publishes the new one. */
        const bool loading = s_mp3_loading && player_scan_busy();
        player_state_t st = player_state();
        /* 光标跟随正在播放的曲目：自动连播（自然结束 / 出错跳曲）由播放器自行
         * 推进 index，UI 必须同步移动高亮，否则列表仍选中上一首，正在播放的那
         * 首反而没有选中。名字校验是必要的兜底 —— 若正在播放的曲目并不属于当前
         * 显示的列表（例如播放途中打开了别的子目录，index 已指向另一首），就不
         * 能用失效的下标去抢光标。 */
        if (!loading && st != PLAYER_IDLE) {
            const int cur = player_current_index();
            if (cur >= 0 && cur < s_mp3_count && cur != s_mp3_sel &&
                strcmp(player_scan_name(cur), player_current_name()) == 0) {
                s_mp3_sel = cur;
            }
        }
        int top = ui_list_top(s_mp3_sel, s_mp3_count, UI_LIST_ROWS);
        const bool sel_changed = (s_mp3_sel != s_paint_mp3_sel)
                                 || (top != s_paint_mp3_top);
        for (int i = 0; i < UI_LIST_ROWS; i++) {
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
                    if (st == PLAYER_PLAYING) {
                        s_mp3_mq.scrolling = false;
                        copy_utf8_clipped(s_pl_name_buf, sizeof(s_pl_name_buf),
                                          strip_ext(player_scan_name(idx)));
                    }
                    else {
                        ui_marquee_step(&s_mp3_mq, s_pl_name_buf,
                                        sizeof(s_pl_name_buf), LIST_LINE_W,
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
                ui_list_row(&s_ui.pl_list, i, idx, s_mp3_count, s_pl_name_buf, sel, false);
            }
            else {
                ui_list_row(&s_ui.pl_list, i, idx, s_mp3_count, "", false, true);
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
        /* Top-right status: playback symbol + repeat mode, e.g. ">>单曲" /
         * "||列表" / "--随机", so the current loop mode is always visible. */
        char stbuf[16];
        snprintf(stbuf, sizeof(stbuf), "%s%s",
                 st == PLAYER_PLAYING ? ">>" : st == PLAYER_PAUSED ? "||" : "--",
                 ui_repeat_text());
        ui_theme_text_set(s_ui.status, stbuf);
        if (st == PLAYER_IDLE) {
            if (player_scan_busy()) {
                ui_pl_prog(UI_STR_LOADING, false);
            }
            else {
                const char *idle_msg = s_mp3_count
                             ? (player_repeat_mode() == PLAYER_REPEAT_ONE
                                ? "循环:单曲"
                                : player_repeat_mode() == PLAYER_REPEAT_RANDOM
                                      ? "循环:随机" : "循环:列表")
                             : "无MP3文件";
                ui_pl_prog(idle_msg, false);
            }
        }
        else {
            /* Now-playing line is driven by ui_player_prog_marquee() every
             * tick (it only touches pl_prog, keeping the main loop free so the
             * B key stays responsive). Re-run it here on a dirty repaint so a
             * track change initialises the scroll immediately. */
            ui_player_prog_marquee();
        }
        if (st == PLAYER_PLAYING) {
            ui_set_hint(UI_STR_HINT_SEEK_PLAY);
        }
        else if (st == PLAYER_PAUSED) {
            ui_set_hint(UI_STR_HINT_SEEK_PAUSE);
        }
        else {
            ui_set_hint(UI_STR_HINT_SEEK_IDLE);
        }
        /* Sticky playback-error toast: while an error is set and nothing is
         * playing, re-arm the toast every refresh so the hint row keeps
         * showing it (e.g. "文件损坏") instead of the normal hint, until the
         * next successful play clears it. */
        if (st == PLAYER_IDLE && player_last_error() != PLAYER_ERR_NONE) {
            set_action(player_err_text(player_last_error()));
        }
}

void ui_player_action(void)
{
        if (s_pv == PV_SOURCE) {
            /* Enter the highlighted source: kick off a background load of that
             * directory, then switch to the track-list view. The order is fixed
             * by the filesystem + sort and cannot be reordered at runtime. */
            if (s_src_count == 0) {
                return;
            }
            player_load(PL_SRC_FOLDER, s_src_path[s_src_sel]);
            s_pv = PV_LIST;
            s_mp3_sel = 0;
            s_paint_mp3_sel = -1;   /* force list repaint */
            s_mp3_loading = true;   /* hide stale list until this load finishes */
            set_action("加载中");
            return;
        }
        if (s_mp3_count == 0) {
            set_action(player_scan_busy() ? UI_STR_LOADING : "无MP3文件");
            return;
        }
        if (player_state() == PLAYER_PLAYING || player_state() == PLAYER_PAUSED) {
            /* Feedback first, then the toggle: the message anticipates the
             * flipped state (toggle always flips). */
            set_action(player_state() == PLAYER_PLAYING ? UI_STR_PAUSED : UI_STR_PLAYING);
            player_toggle();
        }
        else {
            set_action(UI_STR_PLAYING);
            /* 直接用 UI 已知的下标起播，跳过 player_play() 内的路径回查：保证
             * s_index 从第一首起就恒为有效显式下标，避免极端情况下（列表被扫描
             * 任务换源/重排）路径回查失败导致 s_index=-1，进而连播/切歌算错。 */
            player_play_index(s_mp3_sel);
        }
}

void ui_player_adjust(int step)
{
        if (s_pv == PV_SOURCE) {
            if (s_src_count > 0) {
                s_src_sel = (s_src_sel - step + s_src_count) % s_src_count;
            }
            return;
        }
        if (player_state() == PLAYER_PLAYING ||
            player_state() == PLAYER_PAUSED) {
            /* While a track plays, up/down adjusts the output volume
             * (1% steps within 0..10, else 5% — same as the backlight). */
            ui_volume_step(step);
            ui_settings_mark_dirty(SETTINGS_DIRTY_VOLUME);
            char buf[24];
            snprintf(buf, sizeof(buf), "音量 %u%%",
                     (unsigned)hw_audio_get_volume());
            set_action(buf);
        }
        else if (s_mp3_count > 0) {
            s_mp3_sel = (s_mp3_sel - step + s_mp3_count) % s_mp3_count;
        }
}

void ui_player_lr(int dir)
{
    if (ui_nav_current() == UI_PAGE_PLAYER) {
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
}

/* Select: cycle the repeat mode. No toast 鈥?the mode is already shown in
 * the top-right status (">>鍗曟洸" / ">>鍒楄〃"). */
void ui_player_select(void)
{
    ui_mark_dirty();
    player_repeat_toggle();
}

bool ui_player_esc(void)
{
    if (s_pv == PV_SOURCE) {
        return false;               /* leave the page: pop the stack */
    }
    /* PV_LIST: B while playing/paused = stop but stay; while idle,
     * return up one level to the source picker (not the menu). */
    if (player_state() != PLAYER_IDLE) {
        player_stop();
        return true;
    }
    s_pv = PV_SOURCE;
    s_paint_src_sel = -1;   /* force source picker repaint */
    ui_mark_dirty();
    ui_refresh();
    return true;
}

void ui_player_reset_paint(void)
{
    s_paint_mp3_sel = -1;
    s_paint_mp3_top = -1;
    s_paint_src_sel = -1;
}

/* SD card changed under us: re-discover the source folders and, if the
 * track list is showing, suppress the stale rows until the reload lands. */
void ui_player_notify_sd_change(void)
{
    if (ui_nav_current() == UI_PAGE_PLAYER) {
        ui_discover_sources();
        s_paint_src_sel = -1;
    }
    if (s_pv == PV_LIST && player_scan_busy()) {
        s_mp3_loading = true;
    }
}

bool ui_player_panel_open(void)
{
    return s_panel_open;
}

void ui_player_panel_toggle(void)
{
    s_panel_open = !s_panel_open;
    if (s_panel_open) {
        s_panel_sel = 1;             /* default to play/pause */
        lv_obj_clear_flag(s_ui.pl_panel, LV_OBJ_FLAG_HIDDEN);
    }
    else {
        lv_obj_add_flag(s_ui.pl_panel, LV_OBJ_FLAG_HIDDEN);
    }
    ui_mark_dirty();
}

/* While the floating panel is open it owns every key. */
bool ui_player_panel_key(uint32_t key)
{
    if (!s_panel_open) {
        return false;
    }
    if (key == LV_KEY_LEFT) {
        s_panel_sel = (s_panel_sel + PLAYER_PANEL_NBTN - 1) % PLAYER_PANEL_NBTN;
        ui_mark_dirty();          /* force the highlight repaint below */
    }
    else if (key == LV_KEY_RIGHT) {
        s_panel_sel = (s_panel_sel + 1) % PLAYER_PANEL_NBTN;
        ui_mark_dirty();
    }
    else if (key == LV_KEY_ENTER) {
        ui_panel_activate();      /* marks dirty itself */
    }
    else if (key == LV_KEY_ESC) {
        s_panel_open = false;
        lv_obj_add_flag(s_ui.pl_panel, LV_OBJ_FLAG_HIDDEN);
        ui_mark_dirty();
    }
    return true;
}

