/*
 * Page navigation stack.
 *
 * Replaces the old "am I on the main menu?" boolean plus the hand-rolled
 * back-target logic that was spread over the ESC handler: every page used to
 * hard-code where B should go (BT -> Settings, reader -> book list,
 * settings -> menu). The stack records how the user got here instead, so
 * "back" is derived rather than restated per page.
 *
 * The stack holds pages only. Intra-page views (the player's source picker vs
 * track list, the ebook's source picker vs book list) are page-local state and
 * are NOT pushed here — B steps out of them inside the page itself.
 */
#pragma once

#include <stdbool.h>

#include "ui.h"

#define UI_NAV_MAX_DEPTH    4

/* Start at the main menu with an empty stack. */
void ui_nav_init(void);

/* True while the main menu is showing (stack empty). */
bool ui_nav_in_menu(void);

/* The page the user is looking at. Only meaningful when !ui_nav_in_menu(). */
ui_page_t ui_nav_current(void);

/* Depth of the page stack (0 = menu). */
int ui_nav_depth(void);

/* Enter a page: push it and make it current. */
void ui_nav_push(ui_page_t page);

/*
 * Step back one level.
 * Returns true and writes the page to show into `parent` when there is a page
 * underneath; returns false when popping reveals the main menu.
 */
bool ui_nav_pop(ui_page_t *parent);

/* Drop the whole stack and show the main menu. */
void ui_nav_home(void);
