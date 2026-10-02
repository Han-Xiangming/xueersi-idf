/*
 * UI design tokens and primitive constructors.
 *
 * Every colour, spacing and geometry constant the UI draws with lives here, so
 * a visual restyle (P1) touches one file instead of magic numbers scattered
 * across 3000 lines of page code.
 *
 * P0 is deliberately value-preserving: the numbers below are the pre-refactor
 * pixel layout verbatim. This layer is structural only — nothing on screen
 * should change when it lands.
 */
#pragma once

#include <stdint.h>

#include "board_config.h"
#include "lvgl.h"

/* ---- Screen geometry ---- */
#define UI_SCREEN_W         LCD_H_RES
#define UI_SCREEN_H         LCD_V_RES

/* ---- Spacing (4 px grid) ---- */
#define UI_SP_1             4
#define UI_SP_2             8
#define UI_SP_3             12
#define UI_SP_4             16

/* ---- Layout bands ----
 * Frozen from the previous pixel layout: title row, separator under it, and
 * the hint row near the bottom. Pages position themselves against these. */
#define UI_TITLE_X          6       /* 2 px left of the default margin */
#define UI_TITLE_Y          2
#define UI_SEP_Y            34
#define UI_PAGE_MARGIN      8
#define UI_HINT_Y           204

/* ---- Text boxes ---- */
#define UI_TEXT_W           (UI_SCREEN_W - 16)  /* 304: page margin each side */
#define UI_LIST_TEXT_W      (UI_SCREEN_W - 32)  /* 288: cursor column + margin */
/* The persistent battery gauge owns the top-right corner from here on, so any
 * page status text must end before x = UI_BATTERY_ZONE_X. */
#define UI_STATUS_W         238
#define UI_BATTERY_ZONE_X   250

/* ---- Row geometry ---- */
#define UI_LIST_ROWS        6
#define UI_LIST_FIRST_Y     38
#define UI_ROW_H_LIST       26      /* 38, 64, 90, 116, 142, 168 */
#define UI_SETTINGS_FIRST_Y 38
#define UI_ROW_H_SETTING    20      /* 38, 58, 78, 98, 118, 138, 158, 178 */
#define UI_MENU_ROWS        5
#define UI_MENU_FIRST_Y     38
#define UI_ROW_H_MENU       30      /* 38, 68, 98, 128, 158 */

/* ---- Palette (BIOS/DOS style: dark base + cyan accent + gray text) ---- */
#define UI_COLOR_BG         0x000000
#define UI_COLOR_TEXT       0x808080
#define UI_COLOR_ACCENT     0x00E0E0
#define UI_COLOR_TITLE      0xFF8000
#define UI_COLOR_PANEL_BG   0x101010

/* ---- Type ----
 * One embedded CJK bitmap font (~1.2 MB, ASCII + ~22k ideographs + kana).
 * Latin glyphs come from the same face, so the UI never mixes fonts.
 * NOTE: adding a second CJK face would cost ~2 MB of the 3.81 MB app
 * partition — build hierarchy with position/colour/space instead. */
extern const lv_font_t lv_font_cn_16;
#define UI_FONT             (&lv_font_cn_16)

/* ---- Primitives ---- */

/* Hex token -> lv_color_t (lv_color_hex() with a narrower input type). */
lv_color_t ui_theme_color(uint32_t hex);

/* Create a page container: full screen, opaque background, non-scrollable.
 * (The old ui_make_page() took an x offset for a swipe animation that was
 * never enabled; pages are shown/hidden instead.) */
lv_obj_t *ui_theme_page(lv_obj_t *parent);

/* Default label: screen-width minus margins, left-aligned at the page margin. */
lv_obj_t *ui_theme_label(lv_obj_t *parent, const char *text, int y,
                         uint32_t color, lv_text_align_t align);

/* Label with an explicit box — used for columns (settings values, the ebook
 * percentage) that must not be sized by their parent. */
lv_obj_t *ui_theme_label_at(lv_obj_t *parent, const char *text, int x, int y,
                            int w, uint32_t color, lv_text_align_t align);

/* 1 px full-width separator (the line under the title bar). */
lv_obj_t *ui_theme_separator(lv_obj_t *parent, int y);

/* Horizontal progress bar (generic value page). */
lv_obj_t *ui_theme_bar(lv_obj_t *parent, int value);

/* Recolour a label — the focus highlight is a plain text-colour flip. */
void ui_theme_set_color(lv_obj_t *obj, uint32_t color);

/* Set a label's text only when it actually changed. ui_refresh() runs at
 * 60 Hz; pushing an unchanged string re-formats the label and re-dirties its
 * rectangle for nothing. */
void ui_theme_text_set(lv_obj_t *label, const char *text);
