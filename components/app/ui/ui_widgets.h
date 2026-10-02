/*
 * Reusable UI components built on the theme primitives.
 *
 * P0 extracts only what was duplicated verbatim in five places: the
 * "cursor + text" list view used by the music source picker, the track list,
 * the Bluetooth device list and the ebook source/book lists. Each copy had its
 * own scroll-window arithmetic, focus recolouring and blank-row handling;
 * they now share one implementation.
 */
#pragma once

#include <stdbool.h>

#include "ui_theme.h"

/* Maximum rows any list view shows (MP3 / BT / ebook lists all use 6). */
#define UI_LIST_MAX_ROWS    6

/*
 * A list view owns only its row widgets and geometry — the selection index and
 * the data source stay with the owning page, which is what lets the player,
 * ebook and Bluetooth lists keep their own (very different) update rules.
 */
typedef struct {
    lv_obj_t *cursor[UI_LIST_MAX_ROWS];   /* ">" marker column */
    lv_obj_t *text[UI_LIST_MAX_ROWS];     /* row text (clipped / marquee) */
    int       rows;                       /* visible rows */
    int       row_h;                      /* row pitch in px */
    int       first_y;                    /* y of row 0 */
} ui_list_t;

/* Create `rows` cursor+text pairs inside `parent`. */
void ui_list_create(ui_list_t *list, lv_obj_t *parent, int rows,
                    int first_y, int row_h);

/*
 * Scroll window: index of the first visible row. Keeps the selection centred
 * (one row of context above it) and clamps at both ends of the list — the
 * "top = sel - 1" rule every list used before.
 */
int ui_list_top(int sel, int count, int rows);

/*
 * Paint one visible row.
 *  - `blank` (a load is in flight) or an out-of-range `idx` clears the row;
 *  - otherwise the cursor shows ">" for the selected row and the row takes the
 *    accent colour.
 * `text` is whatever the caller wants displayed: the page decides whether that
 * is a clipped name or a marquee frame.
 */
void ui_list_row(ui_list_t *list, int row, int idx, int count,
                 const char *text, bool selected, bool blank);
