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
        list->cursor[i] = NULL;
        list->text[i]   = NULL;
    }

    for (int i = 0; i < rows; i++) {
        const int y = first_y + i * row_h;

        /* Cursor column: always accent-coloured, shows ">" on the selection. */
        lv_obj_t *cur = lv_label_create(parent);
        lv_label_set_text(cur, " ");
        lv_obj_set_pos(cur, 6, y);
        lv_obj_set_style_text_font(cur, UI_FONT, 0);
        lv_obj_set_style_text_color(cur, ui_theme_color(UI_COLOR_ACCENT), 0);
        list->cursor[i] = cur;

        lv_obj_t *txt = lv_label_create(parent);
        lv_label_set_long_mode(txt, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_size(txt, UI_LIST_TEXT_W, LV_SIZE_CONTENT);
        lv_obj_set_pos(txt, 16, y);
        lv_obj_set_style_text_font(txt, UI_FONT, 0);
        lv_obj_set_style_text_color(txt, ui_theme_color(UI_COLOR_TEXT), 0);
        list->text[i] = txt;
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
        ui_theme_text_set(cur, " ");
        ui_theme_text_set(txt, "");
        return;
    }

    ui_theme_text_set(cur, selected ? ">" : " ");
    /* Recolour every pass, not only on a selection change: the underlying list
     * can be swapped underneath us (cache load, live scan, SD hotplug) and a
     * change-gated repaint leaves the accent on the wrong row. */
    ui_theme_set_color(cur, UI_COLOR_ACCENT);
    ui_theme_set_color(txt, selected ? UI_COLOR_ACCENT : UI_COLOR_TEXT);
    ui_theme_text_set(txt, text ? text : "");
}
