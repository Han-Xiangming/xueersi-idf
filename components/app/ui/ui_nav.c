/*
 * Page navigation stack. See ui_nav.h.
 */
#include "ui_nav.h"

static ui_page_t s_stack[UI_NAV_MAX_DEPTH];
static int       s_depth;

void ui_nav_init(void)
{
    s_depth = 0;
}

bool ui_nav_in_menu(void)
{
    return s_depth == 0;
}

ui_page_t ui_nav_current(void)
{
    if (s_depth == 0) {
        return (ui_page_t)0;        /* menu: caller must check ui_nav_in_menu() */
    }
    return s_stack[s_depth - 1];
}

int ui_nav_depth(void)
{
    return s_depth;
}

void ui_nav_push(ui_page_t page)
{
    if (s_depth < UI_NAV_MAX_DEPTH) {
        s_stack[s_depth++] = page;
    }
    else {
        /* Defensive: never overflow. Replace the top instead of corrupting
         * memory; the UI can only ever get here through a bug. */
        s_stack[s_depth - 1] = page;
    }
}

bool ui_nav_pop(ui_page_t *parent)
{
    if (s_depth == 0) {
        return false;               /* already on the menu */
    }
    s_depth--;
    if (s_depth == 0) {
        return false;               /* back to the menu */
    }
    if (parent != NULL) {
        *parent = s_stack[s_depth - 1];
    }
    return true;
}

void ui_nav_home(void)
{
    s_depth = 0;
}
