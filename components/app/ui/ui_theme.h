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
 * Three bands, and every page positions itself against them:
 *   status bar   y 0 .. 33   (page title left, live status + battery right)
 *   content      y 34 .. 215 (lists, reader body, page-specific status row)
 *   key legend   y 216 .. 239 (what the keys do here, or a transient toast)
 * Keeping the three bands in one place is what lets a restyle move the whole
 * layout without hunting for magic numbers inside the pages. */
#define UI_TITLE_X          6       /* 2 px left of the default margin */
#define UI_TITLE_Y          2
#define UI_SEP_Y            34
#define UI_PAGE_MARGIN      8
#define UI_CONTENT_Y        36
#define UI_STATUS_ROW_Y     196     /* page status row (player/book/BT) */
#define UI_LEGEND_Y         218     /* key legend / toast, bottom band */

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
#define UI_ROW_H_SETTING    26      /* 38, 64, 90, 116, 142, 168, 194, 220 (matches list rows) */

/* ---- Launcher grid (3×2 tiles; only 4 populated for now) ----
 * Tiles are 96×84 with 8px gaps; the 3 columns span the full 320px width
 * (8 + 96 + 8 + 96 + 8 + 96 + 8 = 320). Row 0 sits just below the separator;
 * row 1 leaves 2px to the bottom band. All icon/label offsets inside a tile are
 * relative to the tile's top-left. */
#define UI_TILE_W           96
#define UI_TILE_H           84
#define UI_TILE_GAP_X       8
#define UI_TILE_GAP_Y       8
#define UI_TILE_X(col)      (UI_PAGE_MARGIN + (col) * (UI_TILE_W + UI_TILE_GAP_X))
#define UI_TILE_Y(row)      (UI_CONTENT_Y + 2 + (row) * (UI_TILE_H + UI_TILE_GAP_Y))
#define UI_TILE_ICON        40      /* icon box edge inside a tile */
#define UI_TILE_LABEL_Y     54      /* label offset from tile top */
#define UI_TILE_COUNT       6       /* 3 columns × 2 rows */

/* ---- Palette ----
 * Cool dark theme. Two rules drove it:
 *  1. Low-chroma backgrounds. The panel is SPI-driven with a 40-line partial
 *     refresh, so large saturated areas cost real flush time — colour is spent
 *     on small elements (focus bar, status, battery) instead.
 *  2. One accent colour. Without a touchscreen the accent *is* the focus
 *     indicator, so it must never be used decoratively. */
#define UI_COLOR_BG         0x0B0E14   /* screen / page background        */
/* Focused row fill. Deliberately a clear step above the background: on a dim
 * handheld screen the fill — not the text colour — is what the eye finds
 * first when scrolling. */
#define UI_COLOR_PANEL      0x1E2632
#define UI_COLOR_SURFACE    0x151A22   /* cards, floating panel fill      */
#define UI_COLOR_LINE       0x2A323D   /* separators, borders             */
#define UI_COLOR_TEXT       0xE6EDF3   /* primary text                    */
#define UI_COLOR_TEXT_DIM   0x8B949E   /* secondary / unfocused text      */
#define UI_COLOR_ACCENT     0x22D3EE   /* focus + emphasis (one meaning)  */
/* The title bar reads as a title by position and by the separator under it,
 * not by its own colour. */
#define UI_COLOR_TITLE      UI_COLOR_TEXT
#define UI_COLOR_WARN       0xFF9F43
#define UI_COLOR_OK         0x3FB950

/* ---- Type ----
 * One embedded CJK bitmap font (~1.2 MB, ASCII + ~22k ideographs + kana).
 * Latin glyphs come from the same face, so the UI never mixes fonts.
 * NOTE: adding a second CJK face would cost ~2 MB of the 3.81 MB app
 * partition — build hierarchy with position/colour/space instead. */
extern const lv_font_t lv_font_cn_16;
extern const lv_font_t lv_font_pause;
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
