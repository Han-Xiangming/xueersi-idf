/*
 * Ebook pages: source picker + book list + reader
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

/* Reader long-press: holding A past EBOOK_LONG_PRESS_MS starts flipping pages
 * continuously; the repeat interval then accelerates the longer A is held
 * (EBOOK_REPEAT_MS -> EBOOK_REPEAT_MIN_MS, one EBOOK_ACCEL_STEP_MS shrink every
 * EBOOK_ACCEL_EVERY_MS), like a text-scroll accelerate. The first page is
 * turned on the initial press. */
#define EBOOK_LONG_PRESS_MS   400
#define EBOOK_REPEAT_MS       110   /* initial repeat interval after the threshold */
#define EBOOK_REPEAT_MIN_MS   40    /* fastest interval at full speed */
#define EBOOK_ACCEL_STEP_MS   14    /* interval shrink per accel level */
#define EBOOK_ACCEL_EVERY_MS  600   /* shorten the interval this often while held */

static ui_marquee_t s_eb_mq;
/* Long-press auto-flip for the reader's A key. Declared here (ahead of
 * ui_build_ebook_read, which arms the timer) so the timer and its callback are
 * visible at first use; the callback body is defined further below. */
static lv_timer_t *s_eb_auto;
static uint32_t s_eb_a_down_ms;   /* tick when the current A press began */
static void eb_auto_flip_cb(lv_timer_t *t);
/* Ebook book-list page: same 6-row layout as the MP3 page. */
static int s_eb_sel;
static char s_eb_open_name[MP3_NAME_LEN];
/* Reader "jump" overlay: Select enters it, left/right move the target
 * percentage, A jumps, B cancels (see ebook_jump_percent). */
static bool s_eb_jump;
static int  s_eb_jump_pct;

/* Ebook "source" picker, mirroring the music player: <ALL> (the whole
 * /sdcard/eBook tree) + each top-level folder directly under it. Choosing one
 * scans only that folder; its name shows in the status row while browsing. */
typedef enum {
    EBV_SOURCE = 0,   /* picking a source: <ALL> + folders */
    EBV_LIST,         /* browsing the loaded book list */
} ebook_view_t;
EXT_RAM_BSS_ATTR static char s_eb_src_list[SRC_MAX][MP3_NAME_LEN];
EXT_RAM_BSS_ATTR static char s_eb_src_path[SRC_MAX][PLAYER_PATH_LEN];
static int      s_eb_src_count;
static int      s_eb_src_sel;
static ebook_view_t s_ebv = EBV_SOURCE;
static int      s_paint_eb_src_sel = -1;
static int      s_paint_eb_src_top  = -1;
/* True from the moment we leave the source picker to load a source until that
 * load finishes: suppresses the stale book rows of the previous source
 * (mirrors s_mp3_loading). */
static bool     s_eb_loading;
static int s_paint_eb_sel   = -1;
static void copy_book_name(char *dst, size_t dst_size, const char *src)
{
    snprintf(dst, dst_size, "%s", src ? src : "");
}

/* Discover ebook sources: <ALL> (every .txt under /sdcard/eBook, recursive)
 * plus each sub-directory directly under it (one source each), mirroring the
 * music player's ui_discover_sources(). Fills s_eb_src_list[] /
 * s_eb_src_path[]; entry 0 is the whole-tree pseudo-source. Re-run each time
 * the ebook page is (re)built or the SD card changes. */
static void ui_discover_ebook_sources(void)
{
    s_eb_src_count = 0;
    s_eb_src_list[0][0] = '\0';              /* empty name => render as "<ALL>" */
    snprintf(s_eb_src_path[0], sizeof(s_eb_src_path[0]), "%s", EBOOK_ROOT);

    DIR *d = opendir(EBOOK_ROOT);
    if (d != NULL) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL && s_eb_src_count < SRC_MAX - 1) {
            const char *fn = e->d_name;
            if (fn[0] == '.') {
                continue;
            }
            char child[PLAYER_PATH_LEN];
            snprintf(child, sizeof(child), "%s/%s", EBOOK_ROOT, fn);
            struct stat st;
            if (stat(child, &st) != 0 || !S_ISDIR(st.st_mode)) {
                continue;                    /* only sub-directories are sources */
            }
            int idx = s_eb_src_count + 1;
            snprintf(s_eb_src_list[idx], sizeof(s_eb_src_list[idx]), "%s", fn);
            snprintf(s_eb_src_path[idx], sizeof(s_eb_src_path[idx]), "%s", child);
            s_eb_src_count = idx;
        }
        closedir(d);
    }
    s_eb_src_count = (s_eb_src_count == 0) ? 1 : s_eb_src_count + 1;
    if (s_eb_src_sel >= s_eb_src_count) {
        s_eb_src_sel = 0;
    }
}

void ui_build_ebook_list(lv_obj_t *page)
{
    s_eb_sel = 0;
    s_ebv = EBV_SOURCE;
    s_eb_src_sel = 0;
    ui_discover_ebook_sources();

    /* 6 shared list rows: the ebook source picker and the book list reuse
     * them, same as the music player. */
    ui_list_create(&s_ui.eb_list, page, UI_LIST_ROWS, UI_LIST_FIRST_Y, UI_ROW_H_LIST);

    s_ui.eb_status = ui_theme_label(page, UI_STR_SEL_EBOOK, 196, UI_COLOR_TEXT_DIM,
                              LV_TEXT_ALIGN_CENTER);
    s_ui.hint = ui_theme_label(page, UI_STR_HINT_NAV, UI_LEGEND_Y, UI_COLOR_TEXT_DIM,
                         LV_TEXT_ALIGN_CENTER);
}

void ui_build_ebook_read(lv_obj_t *page)
{
    s_eb_jump = false;
    s_eb_jump_pct = 0;

    /* Single body label: the reader engine joins exactly 8 lines with '\n'
     * and measures with the same font, so the layout matches exactly. The
     * 16 px font's natural line height is 30 px; we compress it with a
     * negative line-space so the effective row height becomes 22 px and the
     * widget needs 8*22 = 176 px (y=36..212, just above the y=214 status bar). */
    lv_obj_t *txt = lv_label_create(page);
    lv_label_set_long_mode(txt, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_pos(txt, 0, 36);
    lv_obj_set_size(txt, 320, 176);
    lv_obj_set_style_text_font(txt, UI_FONT, 0);
    lv_obj_set_style_text_color(txt, lv_color_hex(UI_COLOR_TEXT), 0);
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
    lv_obj_set_pos(bar, 8, UI_LEGEND_Y);
    lv_obj_set_style_text_font(bar, UI_FONT, 0);
    lv_obj_set_style_text_color(bar, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_line_space(bar, -8, 0);
    s_ui.eb_bar = bar;

    lv_obj_t *pct = lv_label_create(page);
    lv_label_set_text(pct, "0%");
    lv_label_set_long_mode(pct, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_pos(pct, 272, UI_LEGEND_Y);
    lv_obj_set_size(pct, 40, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(pct, UI_FONT, 0);
    lv_obj_set_style_text_color(pct, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_align(pct, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_line_space(pct, -8, 0);
    s_ui.eb_pct = pct;

    s_ui.hint = ui_theme_label(page, "", UI_LEGEND_Y, UI_COLOR_TEXT,
                         LV_TEXT_ALIGN_CENTER);

    /* Lazily create the long-press auto-flip timer (once); it stays paused
     * until A is held on the reader. */
    if (s_eb_auto == NULL) {
        s_eb_auto = lv_timer_create(eb_auto_flip_cb, EBOOK_REPEAT_MS, NULL);
        lv_timer_pause(s_eb_auto);
    }
}
void ui_refresh_ebook_list(void)
{
        if (s_ebv == EBV_SOURCE) {
            /* Source picker: <ALL> (whole eBook tree) + top-level folders. */
            int top = ui_list_top(s_eb_src_sel, s_eb_src_count, UI_LIST_ROWS);
            const bool sel_changed = (s_eb_src_sel != s_paint_eb_src_sel)
                                   || (top != s_paint_eb_src_top);
            for (int i = 0; i < UI_LIST_ROWS; i++) {
                int idx = top + i;
                const int sel = (idx == s_eb_src_sel);
                if (idx < s_eb_src_count) {
                    static char s_eb_src_buf[MP3_NAME_LEN];
                    const char *label = (s_eb_src_list[idx][0] == '\0')
                                      ? "<ALL>" : s_eb_src_list[idx];
                    copy_utf8_clipped(s_eb_src_buf, sizeof(s_eb_src_buf), label);
                    ui_list_row(&s_ui.eb_list, i, idx, s_eb_src_count, s_eb_src_buf, sel, false);
                }
                else {
                    ui_list_row(&s_ui.eb_list, i, idx, s_eb_src_count, "", false, true);
                }
            }
            if (sel_changed) {
                s_paint_eb_src_sel = s_eb_src_sel;
                s_paint_eb_src_top = top;
            }
            ui_theme_text_set(s_ui.eb_status, UI_STR_SEL_EBOOK);
            ui_set_hint(UI_STR_HINT_NAV);
            return;
        }

        /* EBV_LIST: the book list of the chosen source. */
        int count = ebook_scan_count();
        if (s_eb_sel >= count && count > 0) {
            s_eb_sel = count - 1;
        }
        int top = ui_list_top(s_eb_sel, count, UI_LIST_ROWS);
        const bool sel_changed = (s_eb_sel != s_paint_eb_sel);
        /* While a source load is in flight, the engine still publishes the
         * previous source's list. Suppress drawing it so we don't flash the
         * old books for a frame before the scan publishes the new one. */
        const bool loading = s_eb_loading && ebook_scan_busy();
        for (int i = 0; i < UI_LIST_ROWS; i++) {
            int idx = top + i;
            const int sel = (idx == s_eb_sel);
            if (!loading && idx < count) {
                static char s_eb_name_buf[64];
                copy_book_name(s_eb_name_buf, sizeof(s_eb_name_buf),
                               ebook_scan_name(idx));
                if (sel) {
                    /* Selected row scrolls its name if wider than the line. */
                    ui_marquee_step(&s_eb_mq, s_eb_name_buf,
                                    sizeof(s_eb_name_buf), LIST_LINE_W,
                                    s_eb_name_buf);
                }
                else {
                    /* Same as the MP3 list: never clobber s_eb_mq.scrolling from
                     * a non-selected row (would freeze the marquee after one step).
                     * ui_marquee_step() clears it when the selected name fits. */
                }
                ui_list_row(&s_ui.eb_list, i, idx, count, s_eb_name_buf, sel, false);
            }
            else {
                ui_list_row(&s_ui.eb_list, i, idx, count, "", false, true);
            }
        }
        if (sel_changed) {
            s_paint_eb_sel = s_eb_sel;
        }
        /* The load we armed on leaving the source picker has now published
         * (busy cleared) — stop suppressing the book rows. */
        if (s_eb_loading && !ebook_scan_busy()) {
            s_eb_loading = false;
        }
        if (ebook_scan_busy()) {
            ui_theme_text_set(s_ui.eb_status, "扫描中...");
        }
        else if (count == 0) {
            ui_theme_text_set(s_ui.eb_status, "无TXT文件");
        }
        else {
            char buf[80];
            snprintf(buf, sizeof(buf), "[%s] %d 本",
                     ebook_current_src_name(), count);
            ui_theme_text_set(s_ui.eb_status, buf);
        }
        ui_set_hint(UI_STR_HINT_EBOOK_OPEN);
}

void ui_refresh_ebook_read(void)
{
        ui_theme_text_set(s_ui.title, s_eb_open_name);
        char buf[16];
        int total = ebook_page_count();
        if (total > 0) {
            snprintf(buf, sizeof(buf), "%d/%d", ebook_page(), total);
        }
        else {
            /* Total page count still computing: show just the current page. */
            snprintf(buf, sizeof(buf), "%d", ebook_page());
        }
        ui_theme_text_set(s_ui.status, buf);
        ui_theme_text_set(s_ui.eb_text_label, ebook_page_text());

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
        ui_theme_text_set(s_ui.eb_bar, prog);
        char pbuf[8];
        snprintf(pbuf, sizeof(pbuf), "%d%%", pct);
        ui_theme_text_set(s_ui.eb_pct, pbuf);
        if (s_eb_jump) {
            /* Jump overlay: the hint row shows the adjustable target percent
             * (toasts are suspended while it is active). Hide the status-row
             * bar/percentage so the hint text does not overlap them. */
            char jbuf[48];
            snprintf(jbuf, sizeof(jbuf), "跳转至 %d%% 左/右调 A确认 B取消",
                     s_eb_jump_pct);
            ui_set_hint(jbuf);
            if (s_ui.eb_bar && s_ui.eb_pct) {
                lv_obj_add_flag(s_ui.eb_bar, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_ui.eb_pct, LV_OBJ_FLAG_HIDDEN);
            }
        }
        else {
            ui_set_hint("");
        }
}

void ui_ebook_list_action(void)
{
        if (s_ebv == EBV_SOURCE) {
            /* Enter the highlighted source: kick off a background scan of that
             * directory (or the whole eBook tree for <ALL>), then switch to
             * the book-list view. */
            if (s_eb_src_count == 0) {
                return;
            }
            ebook_scan_root(s_eb_src_path[s_eb_src_sel]);
            s_ebv = EBV_LIST;
            s_eb_sel = 0;
            s_paint_eb_sel = -1;   /* force list repaint */
            s_eb_loading = true;   /* hide stale books until this load finishes */
            set_action("加载中");
            return;
        }
        if (ebook_scan_count() == 0) {
            set_action("无TXT文件");
            return;
        }
        if (!ebook_open(s_eb_sel)) {
            set_action("打开失败");
            return;
        }
        const uint32_t resume = ebook_resume_percent();
        const ebook_resume_t rkind = ebook_resume_kind();
        copy_book_name(s_eb_open_name, sizeof(s_eb_open_name),
                       ebook_scan_name(s_eb_sel));
        ui_go(UI_PAGE_EBOOK_READ);   /* book list -> reader: remember parent */
        if (resume > 0) {
            /* Say how the position was found: an exact restore and a
             * re-location after the file changed are very different news.
             * Kept short — the toast buffer is 32 bytes (CJK = 3 B/glyph). */
            const char *fmt = (rkind == EBOOK_RESUME_EXACT)   ? "已续读 %u%%"
                            : (rkind == EBOOK_RESUME_DRIFT)   ? "已重新定位 %u%%"
                            :                                   "文件已变 跳至 %u%%";
            char buf[32];
            snprintf(buf, sizeof(buf), fmt, (unsigned)resume);
            set_action(buf);
        }
}

/* Long-press auto-flip timer for the reader's A key. Created once (lazily in
 * ui_build_ebook_read) and paused/resumed; the callback polls the physical
 * hold state because A is a single-shot key LVGL never repeats. The timer runs
 * at a fixed EBOOK_REPEAT_MS cadence; the long-press threshold is enforced by
 * s_eb_a_down_ms (avoids depending on lv_timer_reset, which would otherwise
 * fire immediately after resume because the timer's last_run is stale). */
static void eb_auto_flip_cb(lv_timer_t *t)
{
    (void)t;
    /* Stop if we left the reader, the jump overlay took over, A was released,
     * or we reached the last page. */
    if (ui_nav_current() != UI_PAGE_EBOOK_READ || s_eb_jump ||
        !hw_button_is_held(LV_KEY_ENTER)) {
        lv_timer_pause(s_eb_auto);
        return;
    }
    /* Wait out the long-press threshold before flipping continuously. */
    uint32_t held = lv_tick_elaps(s_eb_a_down_ms);
    if (held < EBOOK_LONG_PRESS_MS) {
        return;
    }
    if (ebook_at_end()) {
        set_action("最后一页");
        lv_timer_pause(s_eb_auto);
        return;
    }
    ebook_page_flip(1);
    set_action("下一页");
    ui_mark_dirty();
    ui_refresh();
    /* Accelerate: the longer A stays held, the shorter the repeat interval
     * (down to EBOOK_REPEAT_MIN_MS). */
    int level = (int)(held - EBOOK_LONG_PRESS_MS) / EBOOK_ACCEL_EVERY_MS;
    uint32_t period = EBOOK_REPEAT_MS;
    if (level > 0) {
        int shrink = level * EBOOK_ACCEL_STEP_MS;
        period = (shrink >= (int)(EBOOK_REPEAT_MS - EBOOK_REPEAT_MIN_MS))
                 ? EBOOK_REPEAT_MIN_MS
                 : EBOOK_REPEAT_MS - (uint32_t)shrink;
    }
    lv_timer_set_period(s_eb_auto, period);
}

void ui_ebook_read_action(void)
{
        if (s_eb_jump) {
            /* Confirm the jump target. */
            if (ebook_jump_percent(s_eb_jump_pct)) {
                char buf[24];
                snprintf(buf, sizeof(buf), "已跳转至 %d%%", s_eb_jump_pct);
                set_action(buf);
            }
            s_eb_jump = false;
            return;
        }
        if (ebook_at_end()) {
            set_action("最后一页");
        }
        else {
            ebook_page_flip(1);
            set_action("下一页");
            /* Arm long-press continuous flip: the first page is already turned
             * above; the timer waits EBOOK_LONG_PRESS_MS, then flips repeatedly
             * while A stays held. A short tap releases before it ever fires. */
            if (s_eb_auto != NULL) {
                s_eb_a_down_ms = lv_tick_get();
                lv_timer_resume(s_eb_auto);
            }
        }
}

void ui_ebook_adjust(int step)
{
        if (s_ebv == EBV_SOURCE) {
            if (s_eb_src_count > 0) {
                s_eb_src_sel = (s_eb_src_sel - step + s_eb_src_count) % s_eb_src_count;
            }
            return;
        }
        {
            int count = ebook_scan_count();
            if (count > 0) {
                s_eb_sel = (s_eb_sel - step + count) % count;
            }
        }
}

void ui_ebook_lr(int dir)
{
    if (ui_nav_current() == UI_PAGE_EBOOK_READ) {
        if (s_eb_jump) {
            /* Jump overlay: left/right adjust the target percentage. */
            s_eb_jump_pct += (dir > 0) ? 1 : -1;
            if (s_eb_jump_pct < 0) {
                s_eb_jump_pct = 0;
            }
            if (s_eb_jump_pct > 100) {
                s_eb_jump_pct = 100;
            }
            ui_refresh();
            return;
        }
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
}

/* Select on the reader: open the percentage jump overlay. */
void ui_ebook_select(void)
{
    s_eb_jump = true;
    s_eb_jump_pct = ebook_percent();
    ui_mark_dirty();
}

bool ui_ebook_esc(void)
{
    if (ui_nav_current() == UI_PAGE_EBOOK_READ) {
        if (s_eb_auto != NULL) {
            lv_timer_pause(s_eb_auto);   /* stop any running auto-flip */
        }
        if (s_eb_jump) {
            s_eb_jump = false;         /* cancel, stay on the page */
            ui_mark_dirty();
            ui_refresh();
            return true;
        }
        /* Persist the position right now and report a card that refused
         * the write instead of losing it silently. */
        const bool saved = ebook_progress_flush();
        /* Rebuild the list page, then restore the browsing position so
         * coming back lands exactly where the user left off. */
        const int bk_src_sel = s_eb_src_sel;
        const int bk_sel = s_eb_sel;
        ui_nav_back_or_menu();
        s_ebv = EBV_LIST;
        s_eb_src_sel = bk_src_sel;
        if (s_eb_src_sel >= s_eb_src_count) {
            s_eb_src_sel = 0;          /* source vanished on card: clamp */
        }
        s_eb_sel = bk_sel;
        s_paint_eb_src_sel = -1;
        s_paint_eb_src_top = -1;
        ui_mark_dirty();
        ui_refresh();
        if (!saved) {
            set_action("进度保存失败");
        }
        return true;
    }

    /* Book list: B steps out of the list back to the source picker;
     * on the picker itself B leaves the page (nav stack pops). */
    if (s_ebv == EBV_SOURCE) {
        return false;
    }
    s_ebv = EBV_SOURCE;
    s_paint_eb_src_sel = -1;
    s_paint_eb_src_top = -1;
    ui_mark_dirty();
    ui_refresh();
    return true;
}

void ui_ebook_reset_paint(void)
{
    s_paint_eb_sel = -1;
    s_paint_eb_src_sel = -1;
    s_paint_eb_src_top = -1;
}

/* SD card changed under us: refresh the source folders and reload the
 * book list of the source currently being browsed. */
void ui_ebook_notify_sd_change(void)
{
    if (ui_nav_current() == UI_PAGE_EBOOK_LIST) {
        ui_discover_ebook_sources();
        s_paint_eb_src_sel = -1;
    }
    if (s_ebv == EBV_LIST) {
        ebook_scan_root(s_eb_src_path[s_eb_src_sel]);
        s_eb_loading = true;
    }
}

/* Selected book row marquee (long filename scrolls). */
void ui_ebook_list_marquee(void)
{
    if (!s_eb_mq.scrolling) {
        return;
    }
    int count = ebook_scan_count();
    int top = ui_list_top(s_eb_sel, count, UI_LIST_ROWS);
    int i = s_eb_sel - top;
    if (i < 0 || i >= UI_LIST_ROWS) {
        return;
    }
    static char name[64];
    copy_book_name(name, sizeof(name), ebook_scan_name(s_eb_sel));
    static char out[64];
    ui_marquee_step(&s_eb_mq, out, sizeof(out), LIST_LINE_W, name);
    ui_theme_text_set(s_ui.eb_list.text[i], out);
}
