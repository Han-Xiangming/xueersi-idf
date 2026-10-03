/*
 * Launcher (home screen). Replaces the old text list menu.
 *
 * A 3×2 grid of icon tiles; only the first four slots are populated for now
 * (music / ebook / settings / bluetooth — the 5th and 6th stay empty). The
 * grid is walked with the arrow keys: up/down change rows, left/right change
 * columns, and the nearest valid slot in the requested direction wins. Enter
 * opens the selected page.
 *
 * Page-local state lives here; the assembly layer (ui.c) only calls build /
 * refresh / nav / enter through ui_internal.h.
 */
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "ui.h"
#include "ui_internal.h"
#include "ui_strings.h"
#include "ui_theme.h"
#include "ui_widgets.h"

/* Which slots are live. Empty slots (indices 4 and 5) are simply not created,
 * so the grid reads as four icons in a 3-column frame. */
typedef enum {
    ICON_MUSIC = 0,
    ICON_EBOOK,
    ICON_GEAR,
    ICON_BT,
} launch_icon_t;

typedef struct {
    int           row;
    int           col;
    launch_icon_t icon;
    const char   *name;
    ui_page_t     page;
} launch_slot_t;

static const launch_slot_t s_launch[] = {
    {0, 0, ICON_MUSIC, "音乐",   UI_PAGE_PLAYER},
    {0, 1, ICON_EBOOK, "电子书", UI_PAGE_EBOOK_LIST},
    {0, 2, ICON_GEAR,  "设置",   UI_PAGE_SETTINGS},
    {1, 0, ICON_BT,    "蓝牙",   UI_PAGE_BT},
};
#define LAUNCH_COUNT ((int)(sizeof(s_launch) / sizeof(s_launch[0])))

static int s_launch_sel = 0;

static ui_tiles_t s_tiles;

/* ---- Icons (simple geometry, drawn in neutral dim colour; the tile's focus
 * fill + accent border already carry the selection, so icons need no re-color
 * on focus). Centered at (cx, cy) inside the tile, ~36px footprint. ---- */

static lv_obj_t *s_make_box(lv_obj_t *parent, int x, int y, int w, int h,
                            int radius, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_color(o, ui_theme_color(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static void ui_icon_music(lv_obj_t *p, int cx, int cy)
{
    /* Note head + stem. */
    s_make_box(p, cx - 11, cy + 2, 13, 13, 7, UI_COLOR_TEXT_DIM);   /* head */
    s_make_box(p, cx - 1,  cy - 13, 4, 24, 2, UI_COLOR_TEXT_DIM);  /* stem */
}

static void ui_icon_ebook(lv_obj_t *p, int cx, int cy)
{
    /* Book: rounded rectangle + spine + a few text lines. */
    lv_obj_t *book = s_make_box(p, cx - 16, cy - 16, 32, 32, 4, UI_COLOR_TEXT_DIM);
    lv_obj_set_style_border_side(book, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(book, 2, 0);
    lv_obj_set_style_border_color(book, ui_theme_color(UI_COLOR_BG), 0);
    for (int i = 0; i < 3; i++) {
        s_make_box(p, cx - 11, cy - 8 + i * 8, 18, 2, 0, UI_COLOR_BG);
    }
}

static void ui_icon_gear(lv_obj_t *p, int cx, int cy)
{
    /* Gear: outer ring + hub + four teeth. */
    s_make_box(p, cx - 14, cy - 14, 28, 28, 14, UI_COLOR_TEXT_DIM);
    s_make_box(p, cx - 5,  cy - 5,  10, 10, 5,  UI_COLOR_BG);
    s_make_box(p, cx - 2,  cy - 16, 4,  6,  0,  UI_COLOR_TEXT_DIM);
    s_make_box(p, cx - 2,  cy + 10, 4,  6,  0,  UI_COLOR_TEXT_DIM);
    s_make_box(p, cx - 16, cy - 2,  6,  4,  0,  UI_COLOR_TEXT_DIM);
    s_make_box(p, cx + 10, cy - 2,  6,  4,  0,  UI_COLOR_TEXT_DIM);
}

static void ui_icon_bt(lv_obj_t *p, int cx, int cy)
{
    /* Bluetooth: ring + "BT" caption. */
    s_make_box(p, cx - 15, cy - 15, 30, 30, 15, UI_COLOR_TEXT_DIM);
    lv_obj_t *cap = lv_label_create(p);
    lv_label_set_text(cap, "BT");
    lv_obj_set_pos(cap, cx - 12, cy - 7);
    lv_obj_set_style_text_font(cap, UI_FONT, 0);
    lv_obj_set_style_text_color(cap, ui_theme_color(UI_COLOR_BG), 0);
}

void ui_build_launcher(void)
{
    lv_obj_t *mp = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(mp);
    lv_obj_set_pos(mp, 0, 0);
    lv_obj_set_size(mp, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_style_bg_color(mp, ui_theme_color(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(mp, LV_OPA_COVER, 0);
    lv_obj_clear_flag(mp, LV_OBJ_FLAG_SCROLLABLE);
    s_ui.menu_page = mp;

    /* Title bar. */
    lv_obj_t *title = lv_label_create(mp);
    lv_label_set_text(title, "小喵掌机");
    lv_obj_set_pos(title, UI_TITLE_X, UI_TITLE_Y);
    lv_obj_set_style_text_font(title, UI_FONT, 0);
    lv_obj_set_style_text_color(title, ui_theme_color(UI_COLOR_TITLE), 0);
    ui_theme_separator(mp, UI_SEP_Y);

    /* Tiles at their grid positions (only the live slots). */
    int xs[UI_TILE_COUNT], ys[UI_TILE_COUNT];
    for (int i = 0; i < LAUNCH_COUNT; i++) {
        xs[i] = UI_TILE_X(s_launch[i].col);
        ys[i] = UI_TILE_Y(s_launch[i].row);
    }
    ui_tiles_create(&s_tiles, mp, LAUNCH_COUNT, xs, ys, UI_TILE_W, UI_TILE_H);

    for (int i = 0; i < LAUNCH_COUNT; i++) {
        lv_obj_t *bg = s_tiles.tile[i].bg;
        ui_theme_text_set(s_tiles.tile[i].label, s_launch[i].name);
        const int cx = UI_TILE_W / 2;
        const int cy = UI_TILE_ICON / 2 + 10;
        switch (s_launch[i].icon) {
        case ICON_MUSIC: ui_icon_music(bg, cx, cy); break;
        case ICON_EBOOK: ui_icon_ebook(bg, cx, cy); break;
        case ICON_GEAR:  ui_icon_gear(bg, cx, cy);  break;
        case ICON_BT:    ui_icon_bt(bg, cx, cy);    break;
        }
    }

    /* Bottom band: position indicator left, key legend right. */
    s_ui.menu_status = ui_theme_label_at(mp, "", 4, UI_LEGEND_Y, 120,
                                         UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_LEFT);
    s_ui.hint = ui_theme_label_at(mp, "A:进入  ←→选  ↑↓换排", 130, UI_LEGEND_Y, 186,
                                  UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_RIGHT);
}

void ui_refresh_launcher(void)
{
    ui_tiles_set_focus(&s_tiles, LAUNCH_COUNT, s_launch_sel);

    char buf[32];
    snprintf(buf, sizeof(buf), "[%d/%d]", s_launch_sel + 1, LAUNCH_COUNT);
    ui_theme_text_set(s_ui.menu_status, buf);
}

/* Arrow-key navigation over the grid. Left/right walk the row-major slot order
 * and wrap around at the ends, so pressing left on the first slot (or right on
 * the last) carries onto the other row instead of sticking at the grid edge.
 * Up/down step between rows, preferring the slot nearest the current column. */
void ui_launcher_nav(uint32_t key)
{
    const int cur = s_launch_sel;

    if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT) {
        /* s_launch is stored in row-major order, so stepping the index moves
         * horizontally and rolls over to the next/previous row at the ends. */
        const int step = (key == LV_KEY_RIGHT) ? 1 : LAUNCH_COUNT - 1;
        s_launch_sel = (cur + step) % LAUNCH_COUNT;
        ui_refresh_launcher();
        return;
    }
    if (key != LV_KEY_UP && key != LV_KEY_DOWN) {
        return;
    }

    const int row = s_launch[cur].row;
    const int col = s_launch[cur].col;
    const int tr  = (key == LV_KEY_UP) ? row - 1 : row + 1;

    int best = cur, best_dist = 1000;
    for (int i = 0; i < LAUNCH_COUNT; i++) {
        if (i == cur) {
            continue;
        }
        if (s_launch[i].row != tr) {
            continue;
        }
        const int dc = abs(s_launch[i].col - col);   /* prefer same column */
        if (dc < best_dist) {
            best_dist = dc;
            best = i;
        }
    }
    s_launch_sel = best;
    ui_refresh_launcher();
}

/* Page of the current selection — what ENTER opens. */
ui_page_t ui_launcher_page(void)
{
    return s_launch[s_launch_sel].page;
}

void ui_launcher_reset(void)
{
    s_launch_sel = 0;
}
