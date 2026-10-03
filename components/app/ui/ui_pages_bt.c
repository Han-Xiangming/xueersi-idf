/*
 * Bluetooth page: sink scan / pair / connect
 *
 * Split out of ui.c (P0 structural refactor). Behaviour and pixel layout
 * are unchanged: page-local state lives here, screen-wide state and the
 * shared helpers come from ui.c via ui_internal.h.
 */
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/param.h>
#include <sys/stat.h>

#include "audio.h"
#include "battery.h"
#include "bluetooth_audio.h"
#include "board_config.h"
#include "buttons.h"
#include "ebook.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "lcd.h"
#include "lvgl.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "player.h"
#include "sd.h"
#include "ui.h"
#include "ui_internal.h"
#include "ui_strings.h"
#include "ui_theme.h"
#include "ui_widgets.h"

/* Bluetooth sink picker: same 6-row list layout as the MP3 page. */
static int s_bt_sel;
/* Snapshot of the BT device list so the UI does not re-format device names
 * (incl. the MAC-address fallback) on every 16 ms tick. Refreshed only when
 * bluetooth_audio_device_version() advances. */
static uint32_t s_bt_list_ver;
static int      s_bt_list_cnt;
static char     s_bt_list_name[BT_MAX_DEVICES][BT_DEV_NAME_LEN];
static int s_paint_bt_sel   = -1;
void ui_build_bt(lv_obj_t *page)
{
    /* The Bluetooth stack is lazily brought up here (deferred from boot).
     * Idempotent. */
    bluetooth_audio_enable();
    s_bt_sel = 0;

    ui_list_create(&s_ui.bt_list, page, UI_LIST_ROWS, UI_LIST_FIRST_Y, UI_ROW_H_LIST);

    s_ui.bt_status = ui_theme_label(page, "扫描中...", 196, UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_CENTER);
    s_ui.hint = ui_theme_label(page, "↑↓选 A连接 Select扫描 B返回", UI_LEGEND_Y, UI_COLOR_TEXT_DIM,
                         LV_TEXT_ALIGN_CENTER);

    bluetooth_audio_scan_start();   /* scan once on entry; SELECT re-scans */
}

void ui_refresh_bt(void)
{
        int count = bluetooth_audio_device_count();
        if (s_bt_sel >= count) {
            s_bt_sel = count > 0 ? count - 1 : 0;
        }

        /* Refresh the cached device list only when bluetooth_audio says it changed
         * (a device added, or a nameless device's name arrived). This avoids
         * re-formatting the MAC-address fallback string for every visible row
         * on every 16 ms tick. */
        uint32_t ver = bluetooth_audio_device_version();
        if (ver != s_bt_list_ver) {
            s_bt_list_ver = ver;
            s_bt_list_cnt = count;
            for (int i = 0; i < count && i < BT_MAX_DEVICES; i++) {
                strncpy(s_bt_list_name[i], bluetooth_audio_device_name(i),
                        BT_DEV_NAME_LEN - 1);
                s_bt_list_name[i][BT_DEV_NAME_LEN - 1] = '\0';
            }
        }

        int top = ui_list_top(s_bt_sel, count, UI_LIST_ROWS);
        const bool sel_changed = (s_bt_sel != s_paint_bt_sel);
        for (int i = 0; i < UI_LIST_ROWS; i++) {
            int idx = top + i;
            const int sel = (idx == s_bt_sel);
            if (idx < count) {
                const char *nm = (idx < s_bt_list_cnt && idx < BT_MAX_DEVICES)
                                 ? s_bt_list_name[idx]
                                 : bluetooth_audio_device_name(idx);
                ui_list_row(&s_ui.bt_list, i, idx, count, nm, sel, false);
            }
            else {
                ui_list_row(&s_ui.bt_list, i, idx, count, "", false, true);
            }
        }
        if (sel_changed) {
            s_paint_bt_sel = s_bt_sel;
        }

        if (bluetooth_audio_is_connected()) {
            char st[28];
            snprintf(st, sizeof(st), "已连接 %s", bluetooth_audio_peer_name());
            st[27] = '\0';
            ui_theme_text_set(s_ui.bt_status, st);
            ui_set_hint("A断开 B返回");
            /* Connected sink takes over output: explicitly flip the route so
             * decoded audio goes to Bluetooth instead of the speaker. */
            hw_audio_set_route(AUDIO_ROUTE_BT);
        }
        else if (bluetooth_audio_pair_state() == BT_PAIR_PAIRING) {
            /* Show the SSP passkey so the user can verify it on the sink. */
            char st[28];
            snprintf(st, sizeof(st), "配对码 %06u", (unsigned)bluetooth_audio_passkey());
            st[27] = '\0';
            ui_theme_text_set(s_ui.bt_status, st);
            ui_set_hint("配对中... B返回");
        }
        else if (bluetooth_audio_pair_state() == BT_PAIR_CONNECTING) {
            uint8_t rc = bluetooth_audio_retry_count();
            uint8_t rm = bluetooth_audio_retry_max();
            char st[28];
            if (rc > 0) {
                snprintf(st, sizeof(st), "重试中 %u/%u", (unsigned)rc, (unsigned)rm);
            } else {
                snprintf(st, sizeof(st), "配对中 %s", bluetooth_audio_peer_name());
            }
            st[27] = '\0';
            ui_theme_text_set(s_ui.bt_status, st);
            ui_set_hint("连接中... B返回");
        }
        else if (bluetooth_audio_pair_state() == BT_PAIR_FAIL) {
            ui_theme_text_set(s_ui.bt_status, "配对失败");
            ui_set_hint("A重试 B返回");
        }
        else if (bluetooth_audio_is_scanning()) {
            char st[28];
            snprintf(st, sizeof(st), "扫描中... %d", count);
            st[27] = '\0';
            ui_theme_text_set(s_ui.bt_status, st);
            ui_set_hint("↑↓选 A连接 B返回");
        }
        else {
            if (count) {
                char st[28];
                snprintf(st, sizeof(st), "%d 个设备", count);
                st[27] = '\0';
                ui_theme_text_set(s_ui.bt_status, st);
            }
            else {
                ui_theme_text_set(s_ui.bt_status, "无设备");
            }
            ui_set_hint(count ? "↑↓选 A连接 B返回" : "Select扫描 B返回");
        }
}

void ui_bt_action(void)
{
        if (bluetooth_audio_is_connected()) {
            set_action("断开中");
            bluetooth_audio_disconnect();
            /* Return output to the speaker immediately (the route is explicit;
             * we don't wait for the link to actually drop). */
            hw_audio_set_route(AUDIO_ROUTE_SPEAKER);
        }
        else if (bluetooth_audio_device_count() > 0) {
            set_action("连接中");
            if (!bluetooth_audio_connect_index(s_bt_sel)) {
                set_action("连接失败");
            }
        }
        else {
            set_action("按Select扫描");
        }
}

void ui_bt_adjust(int step)
{
        int count = bluetooth_audio_device_count();
        if (count > 0) {
            s_bt_sel = (s_bt_sel - step + count) % count;
        }
}

/* Select: start a fresh sink scan. */
void ui_bt_select(void)
{
    bluetooth_audio_scan_start();
    set_action("扫描中");
}

void ui_bt_reset_paint(void)
{
    s_paint_bt_sel = -1;
}
