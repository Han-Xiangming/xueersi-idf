/*
 * Software layer: LVGL UI and application logic.
 * See ui.h.
 */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>

#include "board_config.h"
#include "hardware/buttons.h"
#include "hardware/buzzer.h"
#include "hardware/sd.h"
#include "app/ui.h"

#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "lvgl.h"
#include "sdkconfig.h"

/* Build-info macros (read from sdkconfig for the ABOUT page). */
#ifndef CONFIG_IDF_TARGET
#define CONFIG_IDF_TARGET "esp32"
#endif
#ifndef CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ
#define CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ 240
#endif
#ifndef CONFIG_ESPTOOLPY_FLASHFREQ
#define CONFIG_ESPTOOLPY_FLASHFREQ "unknown"
#endif
#ifndef CONFIG_ESPTOOLPY_FLASHSIZE
#define CONFIG_ESPTOOLPY_FLASHSIZE "unknown"
#endif
#ifndef CONFIG_ESPTOOLPY_FLASHMODE
#define CONFIG_ESPTOOLPY_FLASHMODE "unknown"
#endif

#if CONFIG_ESPTOOLPY_FLASHMODE_QIO
#define UI_FLASH_MODE "QIO"
#elif CONFIG_ESPTOOLPY_FLASHMODE_QOUT
#define UI_FLASH_MODE "QOUT"
#elif CONFIG_ESPTOOLPY_FLASHMODE_DIO
#define UI_FLASH_MODE "DIO"
#elif CONFIG_ESPTOOLPY_FLASHMODE_DOUT
#define UI_FLASH_MODE "DOUT"
#else
#define UI_FLASH_MODE CONFIG_ESPTOOLPY_FLASHMODE
#endif

#if CONFIG_ESPTOOLPY_FLASHFREQ_80M
#define UI_FLASH_FREQ "80 MHz"
#elif CONFIG_ESPTOOLPY_FLASHFREQ_40M
#define UI_FLASH_FREQ "40 MHz"
#elif CONFIG_ESPTOOLPY_FLASHFREQ_26M
#define UI_FLASH_FREQ "26 MHz"
#elif CONFIG_ESPTOOLPY_FLASHFREQ_20M
#define UI_FLASH_FREQ "20 MHz"
#else
#define UI_FLASH_FREQ CONFIG_ESPTOOLPY_FLASHFREQ
#endif

#if CONFIG_ESPTOOLPY_FLASHSIZE_4MB
#define UI_FLASH_SIZE "4 MB"
#elif CONFIG_ESPTOOLPY_FLASHSIZE_2MB
#define UI_FLASH_SIZE "2 MB"
#elif CONFIG_ESPTOOLPY_FLASHSIZE_8MB
#define UI_FLASH_SIZE "8 MB"
#elif CONFIG_ESPTOOLPY_FLASHSIZE_16MB
#define UI_FLASH_SIZE "16 MB"
#else
#define UI_FLASH_SIZE CONFIG_ESPTOOLPY_FLASHSIZE
#endif

#if CONFIG_IDF_TARGET_ESP32
#define UI_TARGET_NAME "ESP32"
#else
#define UI_TARGET_NAME CONFIG_IDF_TARGET
#endif

#ifndef CONFIG_SPIRAM_SPEED
#define CONFIG_SPIRAM_SPEED 0
#endif

#define UI_ACTION_MSG_MS            850

static const char *const s_page_names[UI_PAGE_COUNT] = {
    "BUZZER",
    "SD CARD",
    "ABOUT",
};

/* BIOS/DOS-style menu palette: dark base + cyan accent + gray monochrome text. */
static const uint32_t UI_CYAN = 0x00E0E0;
static const uint32_t UI_GRAY = 0x808080;
static const uint32_t UI_BG_DARK = 0x000000;

/* Main-menu layout: up to 5 list rows (first UI_PAGE_COUNT are active). */
#define UI_MENU_ROWS 5
static const int s_menu_y[UI_MENU_ROWS] = {16, 38, 60, 82, 104};

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *page;
    lv_obj_t *title;
    lv_obj_t *value;
    lv_obj_t *sub;
    lv_obj_t *bar;
    lv_obj_t *status;
    lv_obj_t *hint;
    lv_obj_t *accent;

    lv_obj_t *menu_page;
    lv_obj_t *menu_cursor[UI_MENU_ROWS];
    lv_obj_t *menu_text[UI_MENU_ROWS];
    lv_obj_t *menu_status;

    lv_group_t *group;
    ui_page_t page_id;
} ui_state_t;

static ui_state_t s_ui;
static bool s_in_menu;
static int s_menu_sel;
static uint32_t s_buzzer_freq_hz = 988;
static uint32_t s_action_until_ms;
static char s_action[32];

static void copy_text(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) {
        return;
    }
    snprintf(dst, dst_size, "%s", src ? src : "");
}

static void set_action(const char *msg)
{
    copy_text(s_action, sizeof(s_action), msg);
    s_action_until_ms = lv_tick_get() + UI_ACTION_MSG_MS;
}

static const char *short_err(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return "OK";
    case ESP_ERR_TIMEOUT:
        return "TIMEOUT";
    case ESP_ERR_NOT_FOUND:
        return "NOT FOUND";
    case ESP_ERR_INVALID_STATE:
        return "STATE";
    case ESP_ERR_INVALID_ARG:
        return "ARG";
    case ESP_FAIL:
        return "FAIL";
    default:
        return "ERR";
    }
}

static lv_obj_t *ui_label(lv_obj_t *parent,
                          const char *text,
                          int y,
                          uint32_t color,
                          const lv_font_t *font,
                          lv_text_align_t align)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_size(label, LCD_H_RES - 16, LV_SIZE_CONTENT);
    lv_obj_set_pos(label, 8, y);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_align(label, align, 0);
    return label;
}

static lv_obj_t *ui_make_page(int x)
{
    lv_obj_t *page = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(page);
    lv_obj_set_pos(page, x, 0);
    lv_obj_set_size(page, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_color(page, lv_color_hex(UI_BG_DARK), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    if (s_ui.page_id == UI_PAGE_ABOUT) {
        lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(page, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_style_width(page, 3, LV_PART_SCROLLBAR);
        lv_obj_set_style_bg_color(page, lv_color_hex(UI_GRAY), LV_PART_SCROLLBAR);
        lv_obj_set_style_bg_opa(page, LV_OPA_80, LV_PART_SCROLLBAR);
    }
    else {
        lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    }
    return page;
}

static lv_obj_t *ui_bar(lv_obj_t *parent, int value)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, 18, 86);
    lv_obj_set_size(bar, 124, 8);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, value, LV_ANIM_OFF);
    lv_obj_set_style_radius(bar, 4, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_GRAY), 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_CYAN), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
    return bar;
}

static void ui_set_bar(int value)
{
    if (s_ui.bar) {
        lv_bar_set_value(s_ui.bar, MAX(0, MIN(value, 100)), LV_ANIM_ON);
    }
}

static void ui_set_hint(const char *normal)
{
    if (!s_ui.hint) {
        return;
    }
    if (s_action_until_ms && (int32_t)(s_action_until_ms - lv_tick_get()) > 0) {
        lv_label_set_text(s_ui.hint, s_action);
    }
    else {
        s_action_until_ms = 0;
        lv_label_set_text(s_ui.hint, normal);
    }
}

static unsigned ui_kb(size_t bytes)
{
    return (unsigned)((bytes + 512) / 1024);
}

static void ui_build_about_page(lv_obj_t *page)
{
    esp_chip_info_t chip_info;
    char details[1200];

    esp_chip_info(&chip_info);

    const size_t sram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    const size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

    snprintf(details,
             sizeof(details),
             "Model\n"
             "  Xiaomiao Handheld\n"
             "  ESP32-WROVER-B\n"
             "  Author: ZYoung\n\n"
             "CPU\n"
             "  Xtensa LX6\n"
             "    %d MHz x%u\n"
             "  Chip rev: %u\n\n"
             "System\n"
             "  ESP-IDF: %s\n"
             "  FreeRTOS: %s\n"
             "  Target: %s\n"
             "  Build: %s\n\n"
             "Clocks\n"
             "  Flash: %s %s\n"
             "  PSRAM: %d MHz\n"
             "  LCD SPI2: %u MHz\n"
             "  SD SPI2: %u MHz\n\n"
             "Storage\n"
             "  Flash: %s\n"
             "  SRAM: %u KB\n"
             "  PSRAM: %u KB\n\n"
             "Display\n"
             "  ST7735 160x128\n"
             "  SPI2 %u MHz\n"
             "  RGB565 DMA x%u\n"
             "  LVGL %d.%d.%d\n\n"
             "Board IO\n"
             "  Keys: 6 active-low\n"
             "  SD: SPI2 CS22\n"
             "  PWM: GPIO14 Buzzer\n\n"
             "wechat/tel:\n"
             "  15657325738\n",
             CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
             (unsigned)chip_info.cores,
             (unsigned)chip_info.revision,
             esp_get_idf_version(),
             tskKERNEL_VERSION_NUMBER,
             UI_TARGET_NAME,
             __DATE__,
             UI_FLASH_MODE,
             UI_FLASH_FREQ,
             CONFIG_SPIRAM_SPEED,
             (unsigned)(LCD_PIXEL_CLOCK_HZ / 1000000),
             (unsigned)(SD_SPI_MAX_FREQ_KHZ / 1000),
             UI_FLASH_SIZE,
             ui_kb(sram_total),
             ui_kb(psram_total),
             (unsigned)(LCD_PIXEL_CLOCK_HZ / 1000000),
             (unsigned)LCD_DRAW_BUF_COUNT,
             LVGL_VERSION_MAJOR,
             LVGL_VERSION_MINOR,
             LVGL_VERSION_PATCH);

    lv_obj_t *label = lv_label_create(page);
    lv_label_set_text(label, details);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_pos(label, 8, 28);
    lv_obj_set_width(label, LCD_H_RES - 22);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_GRAY), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_line_space(label, 1, 0);

    s_ui.value = lv_label_create(page);
    lv_obj_add_flag(s_ui.value, LV_OBJ_FLAG_HIDDEN);
    s_ui.sub = lv_label_create(page);
    lv_obj_add_flag(s_ui.sub, LV_OBJ_FLAG_HIDDEN);
    s_ui.hint = lv_label_create(page);
    lv_obj_add_flag(s_ui.hint, LV_OBJ_FLAG_HIDDEN);
    s_ui.bar = NULL;
}

static void ui_build_page_content(lv_obj_t *page)
{
    char idx[10];

    s_ui.title = ui_label(page, s_page_names[s_ui.page_id], 7, UI_CYAN, &lv_font_montserrat_14, LV_TEXT_ALIGN_LEFT);
    snprintf(idx, sizeof(idx), "%02u/%02u", (unsigned)s_ui.page_id + 1, (unsigned)UI_PAGE_COUNT);
    s_ui.status = ui_label(page, idx, 7, UI_GRAY, &lv_font_montserrat_10, LV_TEXT_ALIGN_RIGHT);

    /* Header separator, matching the main-menu style. */
    lv_obj_t *sep = lv_obj_create(page);
    lv_obj_remove_style_all(sep);
    lv_obj_set_pos(sep, 0, 22);
    lv_obj_set_size(sep, LCD_H_RES, 1);
    lv_obj_set_style_bg_color(sep, lv_color_hex(UI_GRAY), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);

    if (s_ui.page_id == UI_PAGE_ABOUT) {
        ui_build_about_page(page);
        return;
    }

    s_ui.value = ui_label(page, "--", 38, UI_CYAN, &lv_font_montserrat_14, LV_TEXT_ALIGN_CENTER);
    s_ui.sub = ui_label(page, "--", 63, UI_GRAY, &lv_font_montserrat_10, LV_TEXT_ALIGN_CENTER);
    s_ui.bar = ui_bar(page, 0);
    s_ui.hint = ui_label(page, "U/D Hz  A beep  B menu", 106, UI_GRAY, &lv_font_montserrat_10, LV_TEXT_ALIGN_CENTER);
}

/* Menu uses show/hide transitions instead of LVGL swipe animations. */

static void ui_refresh_menu(void)
{
    for (int i = 0; i < UI_MENU_ROWS; i++) {
        if (i >= UI_PAGE_COUNT) {
            continue;
        }
        const int sel = (i == s_menu_sel);
        lv_label_set_text(s_ui.menu_cursor[i], sel ? ">" : " ");
        lv_obj_set_style_text_color(s_ui.menu_cursor[i], lv_color_hex(UI_CYAN), 0);
        lv_obj_set_style_text_color(s_ui.menu_text[i],
                                    lv_color_hex(sel ? UI_CYAN : UI_GRAY), 0);
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "[%d/%d]", (int)(s_menu_sel + 1), (int)UI_PAGE_COUNT);
    lv_label_set_text(s_ui.menu_status, buf);
}

static void ui_build_menu(void)
{
    lv_obj_t *mp = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(mp);
    lv_obj_set_pos(mp, 0, 0);
    lv_obj_set_size(mp, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_color(mp, lv_color_hex(UI_BG_DARK), 0);
    lv_obj_set_style_bg_opa(mp, LV_OPA_COVER, 0);
    lv_obj_clear_flag(mp, LV_OBJ_FLAG_SCROLLABLE);
    s_ui.menu_page = mp;

    /* Title bar: "- XiaoMiao -" in cyan, left-aligned. */
    lv_obj_t *title = lv_label_create(mp);
    lv_label_set_text(title, "- XiaoMiao -");
    lv_obj_set_pos(title, 4, 2);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_CYAN), 0);

    /* Separator line spanning the full width, clear of the title. */
    lv_obj_t *sep = lv_obj_create(mp);
    lv_obj_remove_style_all(sep);
    lv_obj_set_pos(sep, 0, 14);
    lv_obj_set_size(sep, LCD_H_RES, 1);
    lv_obj_set_style_bg_color(sep, lv_color_hex(UI_GRAY), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);

    /* Menu rows: cursor at x=8, text at x=18. */
    for (int i = 0; i < UI_MENU_ROWS; i++) {
        lv_obj_t *cur = lv_label_create(mp);
        lv_label_set_text(cur, " ");
        lv_obj_set_pos(cur, 8, s_menu_y[i]);
        lv_obj_set_style_text_font(cur, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(cur, lv_color_hex(UI_GRAY), 0);
        s_ui.menu_cursor[i] = cur;

        lv_obj_t *txt = lv_label_create(mp);
        lv_label_set_long_mode(txt, LV_LABEL_LONG_MODE_CLIP);
        lv_label_set_text(txt, i < UI_PAGE_COUNT ? s_page_names[i] : "");
        lv_obj_set_pos(txt, 18, s_menu_y[i]);
        lv_obj_set_style_text_font(txt, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(txt, lv_color_hex(UI_GRAY), 0);
        s_ui.menu_text[i] = txt;
    }

    /* Bottom status bar: "[n/3]" left, "A:OK B:BK" right, both gray. */
    s_ui.menu_status = lv_label_create(mp);
    lv_label_set_text(s_ui.menu_status, "[1/3]");
    lv_obj_set_pos(s_ui.menu_status, 4, 114);
    lv_obj_set_style_text_font(s_ui.menu_status, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_ui.menu_status, lv_color_hex(UI_GRAY), 0);

    lv_obj_t *hint = lv_label_create(mp);
    lv_label_set_text(hint, "A:OK B:BK");
    lv_obj_set_pos(hint, 104, 114);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(UI_GRAY), 0);
}

static void ui_show_menu(void)
{
    s_in_menu = true;
    if (s_ui.page) {
        lv_obj_add_flag(s_ui.page, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(s_ui.menu_page, LV_OBJ_FLAG_HIDDEN);
    ui_refresh_menu();
}

static void ui_enter_page(ui_page_t page)
{
    s_in_menu = false;
    lv_obj_add_flag(s_ui.menu_page, LV_OBJ_FLAG_HIDDEN);
    if (s_ui.page) {
        lv_obj_delete(s_ui.page);
        s_ui.page = NULL;
    }
    s_ui.page_id = page;
    s_ui.page = ui_make_page(0);
    s_ui.title = NULL;
    s_ui.value = NULL;
    s_ui.sub = NULL;
    s_ui.bar = NULL;
    s_ui.status = NULL;
    s_ui.hint = NULL;
    s_ui.accent = NULL;
    ui_build_page_content(s_ui.page);
    ui_refresh();
    if (hw_buzzer_ready()) {
        hw_buzzer_beep(660, 30);
    }
}

void ui_refresh(void)
{
    if (s_in_menu) {
        return;
    }
    if (!s_ui.value || !s_ui.sub || !s_ui.hint) {
        return;
    }

    switch (s_ui.page_id) {
    case UI_PAGE_BUZZER:
        lv_label_set_text_fmt(s_ui.value, "%lu Hz", (unsigned long)s_buzzer_freq_hz);
        lv_label_set_text(s_ui.sub, hw_buzzer_ready() ? "GPIO14 PWM" : "PWM INIT FAIL");
        ui_set_hint("U/D Hz  A beep  B stop");
        ui_set_bar((int)((s_buzzer_freq_hz - 440) * 100 / (1760 - 440)));
        break;
    case UI_PAGE_SD:
        lv_label_set_text(s_ui.value, hw_sd_is_mounted() ? "MOUNTED" : "NO CARD");
        if (hw_sd_is_mounted()) {
            lv_label_set_text_fmt(s_ui.sub, "%s  %luMB", hw_sd_name(), (unsigned long)hw_sd_mb());
        }
        else {
            lv_label_set_text_fmt(s_ui.sub, "GPIO22 CS  %s", short_err(hw_sd_last_err()));
        }
        ui_set_hint(hw_sd_is_mounted() ? "B menu" : "A rescan  B menu");
        ui_set_bar(hw_sd_is_mounted() ? 100 : 0);
        break;
    case UI_PAGE_ABOUT:
        break;
    default:
        break;
    }
}

static void ui_action(void)
{
    esp_err_t err = ESP_OK;

    switch (s_ui.page_id) {
    case UI_PAGE_BUZZER:
        hw_buzzer_beep(s_buzzer_freq_hz, 140);
        set_action(hw_buzzer_ready() ? "Beep" : "Buzzer init fail");
        break;
    case UI_PAGE_SD:
        hw_sd_try_mount();
        err = hw_sd_is_mounted() ? ESP_OK : hw_sd_last_err();
        set_action(hw_sd_is_mounted() ? "SD mounted" : "No SD card");
        break;
    default:
        break;
    }

    if (err == ESP_OK && s_ui.page_id != UI_PAGE_BUZZER) {
        hw_buzzer_beep(660, 35);
    }
    ui_refresh();
}

/* B (ESC) returns to the main menu via ui_show_menu(). */

static void ui_adjust(int step)
{
    switch (s_ui.page_id) {
    case UI_PAGE_BUZZER: {
        int freq = (int)s_buzzer_freq_hz + step * 110;
        s_buzzer_freq_hz = MAX(440, MIN(freq, 1760));
        set_action("Pitch set");
        break;
    }
    default:
        return;
    }
    ui_refresh();
}

static void ui_scroll_about(int step)
{
    if (!s_ui.page) {
        return;
    }

    const int32_t scroll_step = 26;
    const int32_t scroll_y = lv_obj_get_scroll_y(s_ui.page) + step * scroll_step;
    lv_obj_scroll_to_y(s_ui.page, MAX(0, scroll_y), LV_ANIM_ON);
}

static void ui_key_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_KEY) {
        return;
    }

    const uint32_t key = lv_event_get_key(e);

    if (s_in_menu) {
        if (key == LV_KEY_UP) {
            s_menu_sel = (s_menu_sel + UI_PAGE_COUNT - 1) % UI_PAGE_COUNT;
            ui_refresh_menu();
            if (hw_buzzer_ready()) {
                hw_buzzer_beep(990, 16);
            }
        }
        else if (key == LV_KEY_DOWN) {
            s_menu_sel = (s_menu_sel + 1) % UI_PAGE_COUNT;
            ui_refresh_menu();
            if (hw_buzzer_ready()) {
                hw_buzzer_beep(990, 16);
            }
        }
        else if (key == LV_KEY_ENTER) {
            ui_enter_page((ui_page_t)s_menu_sel);
        }
        /* B (ESC) is a no-op while already on the main menu. */
        return;
    }

    /* Inside a detail page. */
    if (key == LV_KEY_ESC) {
        ui_show_menu();
        if (hw_buzzer_ready()) {
            hw_buzzer_beep(520, 24);
        }
        return;
    }
    if (key == LV_KEY_ENTER) {
        ui_action();
        return;
    }
    if (key == LV_KEY_UP) {
        if (s_ui.page_id == UI_PAGE_ABOUT) {
            ui_scroll_about(-1);
        }
        else {
            ui_adjust(1);
        }
        return;
    }
    if (key == LV_KEY_DOWN) {
        if (s_ui.page_id == UI_PAGE_ABOUT) {
            ui_scroll_about(1);
        }
        else {
            ui_adjust(-1);
        }
        return;
    }
}

void ui_create(lv_group_t *group)
{
    s_ui.group = group;
    s_ui.page_id = UI_PAGE_BUZZER;
    s_in_menu = true;
    s_menu_sel = 0;

    s_ui.screen = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_ui.screen);
    lv_obj_set_size(s_ui.screen, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_color(s_ui.screen, lv_color_hex(UI_BG_DARK), 0);
    lv_obj_set_style_bg_opa(s_ui.screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_ui.screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ui.screen, LV_OBJ_FLAG_CLICKABLE);
    lv_group_add_obj(group, s_ui.screen);
    lv_group_focus_obj(s_ui.screen);
    lv_obj_add_event_cb(s_ui.screen, ui_key_event_cb, LV_EVENT_KEY, NULL);

    ui_build_menu();
    ui_refresh_menu();
}

lv_group_t *ui_input_init(lv_display_t *display)
{
    lv_group_t *group = lv_group_create();
    assert(group);
    lv_group_set_default(group);

    lv_indev_t *indev = lv_indev_create();
    assert(indev);
    lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_display(indev, display);
    lv_indev_set_group(indev, group);
    lv_indev_set_read_cb(indev, hw_buttons_read);
    lv_indev_set_long_press_time(indev, 360);
    lv_indev_set_long_press_repeat_time(indev, 130);

    return group;
}

static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

void ui_start_tick_timer(void)
{
    const esp_timer_create_args_t tick_timer_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_timer_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, LVGL_TICK_PERIOD_MS * 1000));
}
