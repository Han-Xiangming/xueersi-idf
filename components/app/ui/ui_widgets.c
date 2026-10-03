/*
 * Reusable UI components. See ui_widgets.h.
 */
#include "ui_widgets.h"

void ui_list_create(ui_list_t *list, lv_obj_t *parent, int rows,
                    int first_y, int row_h)
{
    if (rows > UI_LIST_MAX_ROWS) {
        rows = UI_LIST_MAX_ROWS;
    }
    list->rows   = rows;
    list->row_h  = row_h;
    list->first_y = first_y;

    for (int i = 0; i < UI_LIST_MAX_ROWS; i++) {
        list->row_bg[i] = NULL;
        list->cursor[i] = NULL;
        list->text[i]   = NULL;
    }

    for (int i = 0; i < rows; i++) {
        const int y = first_y + i * row_h;

        /* Row container carrying the focus fill. Transparent until the row is
         * selected; the cursor and the text live inside it, so their
         * coordinates are row-relative. */
        lv_obj_t *row = lv_obj_create(parent);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, y);
        lv_obj_set_size(row, UI_SCREEN_W, row_h);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        /* The 16px font's line_height (30) is taller than most row heights, and
         * LVGL clips children to the parent by default — which would shave the
         * bottom of every label. Let the text paint past the row box; it stays
         * inside the screen and never reaches the next row. */
        lv_obj_add_flag(row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        list->row_bg[i] = row;

        /* Cursor column: accent-coloured, shows ">" on the selection. */
        lv_obj_t *cur = lv_label_create(row);
        lv_label_set_text(cur, " ");
        lv_obj_set_pos(cur, 6, 0);
        lv_obj_set_style_text_font(cur, UI_FONT, 0);
        lv_obj_set_style_text_color(cur, ui_theme_color(UI_COLOR_ACCENT), 0);
        list->cursor[i] = cur;

        lv_obj_t *txt = lv_label_create(row);
        lv_label_set_long_mode(txt, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_size(txt, UI_LIST_TEXT_W, LV_SIZE_CONTENT);
        lv_obj_set_pos(txt, 16, 0);
        lv_obj_set_style_text_font(txt, UI_FONT, 0);
        lv_obj_set_style_text_color(txt, ui_theme_color(UI_COLOR_TEXT_DIM), 0);
        list->text[i] = txt;
    }
}

/* Paint the focus fill under one row. Triple-encoded on purpose (fill +
 * ">" cursor + accent text): with no touchscreen the fill is what the eye
 * catches first when scrolling a list. */
static void ui_list_row_focus(ui_list_t *list, int row, bool on)
{
    lv_obj_t *bg = list->row_bg[row];
    if (bg == NULL) {
        return;
    }
    if (on) {
        lv_obj_set_style_bg_color(bg, ui_theme_color(UI_COLOR_PANEL), 0);
        lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
    }
    else {
        lv_obj_set_style_bg_opa(bg, LV_OPA_TRANSP, 0);
    }
}

int ui_list_top(int sel, int count, int rows)
{
    int top = sel - 1;
    if (top < 0) {
        top = 0;
    }
    if (top > count - rows) {
        top = count - rows;
    }
    if (top < 0) {
        top = 0;
    }
    return top;
}

void ui_list_row(ui_list_t *list, int row, int idx, int count,
                 const char *text, bool selected, bool blank)
{
    if (list == NULL || row < 0 || row >= list->rows) {
        return;
    }
    lv_obj_t *cur = list->cursor[row];
    lv_obj_t *txt = list->text[row];

    if (blank || idx < 0 || idx >= count) {
        ui_list_row_focus(list, row, false);
        ui_theme_text_set(cur, " ");
        ui_theme_text_set(txt, "");
        return;
    }

    ui_list_row_focus(list, row, selected);
    ui_theme_text_set(cur, selected ? ">" : " ");
    /* Recolour every pass, not only on a selection change: the underlying list
     * can be swapped underneath us (cache load, live scan, SD hotplug) and a
     * change-gated repaint leaves the accent on the wrong row. */
    ui_theme_set_color(cur, UI_COLOR_ACCENT);
    ui_theme_set_color(txt, selected ? UI_COLOR_ACCENT : UI_COLOR_TEXT_DIM);
    ui_theme_text_set(txt, text ? text : "");
}

/* ---- Launcher tiles --------------------------------------------------------- */

void ui_tiles_create(ui_tiles_t *t, lv_obj_t *parent, int count,
                     const int x[UI_TILE_COUNT], const int y[UI_TILE_COUNT],
                     int w, int h)
{
    if (t == NULL || parent == NULL) {
        return;
    }
    t->count = count;
    for (int i = 0; i < count; i++) {
        lv_obj_t *bg = lv_obj_create(parent);
        lv_obj_remove_style_all(bg);
        lv_obj_set_pos(bg, x[i], y[i]);
        lv_obj_set_size(bg, w, h);
        lv_obj_set_style_radius(bg, 8, 0);
        lv_obj_set_style_bg_color(bg, ui_theme_color(UI_COLOR_SURFACE), 0);
        lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(bg, ui_theme_color(UI_COLOR_LINE), 0);
        lv_obj_set_style_border_width(bg, 1, 0);
        lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
        t->tile[i].bg = bg;

        lv_obj_t *lab = lv_label_create(bg);
        lv_label_set_text(lab, "");
        lv_obj_set_pos(lab, 0, UI_TILE_LABEL_Y);
        lv_obj_set_size(lab, w, LV_SIZE_CONTENT);
        lv_obj_set_style_text_font(lab, UI_FONT, 0);
        lv_obj_set_style_text_color(lab, ui_theme_color(UI_COLOR_TEXT_DIM), 0);
        lv_obj_set_style_text_align(lab, LV_TEXT_ALIGN_CENTER, 0);
        t->tile[i].label = lab;
    }
}

void ui_tiles_set_focus(const ui_tiles_t *t, int count, int sel)
{
    if (t == NULL) {
        return;
    }
    for (int i = 0; i < count; i++) {
        lv_obj_t *bg = t->tile[i].bg;
        if (i == sel) {
            lv_obj_set_style_bg_color(bg, ui_theme_color(UI_COLOR_PANEL), 0);
            lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(bg, ui_theme_color(UI_COLOR_ACCENT), 0);
            lv_obj_set_style_border_width(bg, 2, 0);
            lv_obj_set_style_text_color(t->tile[i].label, ui_theme_color(UI_COLOR_ACCENT), 0);
        }
        else {
            lv_obj_set_style_bg_color(bg, ui_theme_color(UI_COLOR_SURFACE), 0);
            lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(bg, ui_theme_color(UI_COLOR_LINE), 0);
            lv_obj_set_style_border_width(bg, 1, 0);
            lv_obj_set_style_text_color(t->tile[i].label, ui_theme_color(UI_COLOR_TEXT_DIM), 0);
        }
    }
}
