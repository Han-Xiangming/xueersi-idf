/*
 * UI design tokens and primitive constructors. See ui_theme.h.
 */
#include <string.h>

#include "ui_theme.h"

lv_color_t ui_theme_color(uint32_t hex)
{
    return lv_color_hex(hex);
}

lv_obj_t *ui_theme_page(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_size(page, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_style_bg_color(page, ui_theme_color(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    return page;
}

lv_obj_t *ui_theme_label_at(lv_obj_t *parent, const char *text, int x, int y,
                            int w, uint32_t color, lv_text_align_t align)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text ? text : "");
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_size(label, w, LV_SIZE_CONTENT);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_color(label, ui_theme_color(color), 0);
    lv_obj_set_style_text_font(label, UI_FONT, 0);
    lv_obj_set_style_text_align(label, align, 0);
    return label;
}

lv_obj_t *ui_theme_label(lv_obj_t *parent, const char *text, int y,
                         uint32_t color, lv_text_align_t align)
{
    return ui_theme_label_at(parent, text, UI_PAGE_MARGIN, y, UI_TEXT_W,
                             color, align);
}

lv_obj_t *ui_theme_separator(lv_obj_t *parent, int y)
{
    lv_obj_t *sep = lv_obj_create(parent);
    lv_obj_remove_style_all(sep);
    lv_obj_set_pos(sep, 0, y);
    lv_obj_set_size(sep, UI_SCREEN_W, 1);
    lv_obj_set_style_bg_color(sep, ui_theme_color(UI_COLOR_LINE), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_clear_flag(sep, LV_OBJ_FLAG_SCROLLABLE);
    return sep;
}

lv_obj_t *ui_theme_bar(lv_obj_t *parent, int value)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, 18, 120);
    lv_obj_set_size(bar, 284, 8);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, value, LV_ANIM_OFF);
    lv_obj_set_style_radius(bar, 4, 0);
    lv_obj_set_style_bg_color(bar, ui_theme_color(UI_COLOR_LINE), 0);
    lv_obj_set_style_bg_color(bar, ui_theme_color(UI_COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
    return bar;
}

void ui_theme_set_color(lv_obj_t *obj, uint32_t color)
{
    if (obj == NULL) {
        return;
    }
    lv_obj_set_style_text_color(obj, ui_theme_color(color), 0);
}

void ui_theme_text_set(lv_obj_t *label, const char *text)
{
    if (label == NULL || text == NULL) {
        return;
    }
    const char *cur = lv_label_get_text(label);
    if (cur != NULL && strcmp(cur, text) == 0) {
        return;                     /* unchanged — skip the LVGL set */
    }
    lv_label_set_text(label, text);
}
