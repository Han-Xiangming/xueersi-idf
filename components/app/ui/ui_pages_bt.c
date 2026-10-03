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

/* NVS namespace/key for the Bluetooth master switch (reuses the "bt_on" key
 * the settings page used to own, so a previously-saved preference carries
 * over). */
#define UI_NVS_NS  "ui_cfg"
#define UI_NVS_BT  "bt_on"

/* Bluetooth sink picker: same 6-row list layout as the MP3 page, but with a
 * pinned master-switch row at the top (index 0). Device rows follow at
 * index 1..N, so a device's real list index is (s_bt_sel - 1). */
static int  s_bt_sel;
/* Bluetooth master switch: radio up/down. Persisted; off by default so the
 * device stays silent at boot (the stack is brought up lazily on page entry
 * when this is true). */
static bool s_bt_on;
/* Snapshot of the BT device list so the UI does not re-format device names
 * (incl. the MAC-address fallback) on every 16 ms tick. Refreshed only when
 * bluetooth_audio_device_version() advances. */
static uint32_t s_bt_list_ver;
static int      s_bt_list_cnt;
static char     s_bt_list_name[BT_MAX_DEVICES][BT_DEV_NAME_LEN];
void ui_build_bt(lv_obj_t *page)
{
    s_bt_sel = 0;

    /* Restore the persisted master switch. Off by default (silent at boot);
     * the radio is only powered up below if the switch is on. */
    s_bt_on = false;
    nvs_handle_t h;
    if (nvs_open(UI_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        int32_t v = 0;
        if (nvs_get_i32(h, UI_NVS_BT, &v) == ESP_OK) {
            s_bt_on = (v != 0);
        }
        nvs_close(h);
    }

    if (s_bt_on) {
        /* Lazily bring the stack up (idempotent) and scan once on entry;
         * SELECT re-scans. */
        bluetooth_audio_enable();
        bluetooth_audio_scan_start();
    } else {
        /* Ensure the radio is down if it was left up by a previous session. */
        bluetooth_audio_disable();
    }

    ui_list_create(&s_ui.bt_list, page, UI_LIST_ROWS, UI_LIST_FIRST_Y, UI_ROW_H_LIST);

    s_ui.bt_status = ui_theme_label(page, s_bt_on ? "扫描中..." : "蓝牙已关闭",
                                    196, UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_CENTER);
    s_ui.hint = ui_theme_label(page,
                     s_bt_on ? "↑↓选 A开关/连接 Select扫描 B返回"
                             : "A开关 B返回",
                     UI_LEGEND_Y, UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_CENTER);
}

void ui_refresh_bt(void)
{
        /* Row 0 is the pinned master switch; below it live the device rows. */
        {
            static char sw[32];
            snprintf(sw, sizeof(sw), "蓝牙     %s", s_bt_on ? "开" : "关");
            ui_list_row(&s_ui.bt_list, 0, 0, 1, sw, (s_bt_sel == 0), false);
        }

        if (!s_bt_on) {
            /* Radio off: hide the device list and say so. */
            for (int i = 1; i < UI_LIST_ROWS; i++) {
                ui_list_row(&s_ui.bt_list, i, -1, 0, "", false, true);
            }
            ui_theme_text_set(s_ui.bt_status, "蓝牙已关闭");
            ui_set_hint("A开关 B返回");
            return;
        }

        int count = bluetooth_audio_device_count();
        int total = 1 + count;
        if (s_bt_sel > total - 1) {
            s_bt_sel = total - 1;
        }
        if (s_bt_sel < 0) {
            s_bt_sel = 0;
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

        /* Devices occupy list rows 1..(UI_LIST_ROWS-1); the switch stays pinned at
         * row 0. The device selection index is (s_bt_sel - 1). */
        int dev_sel = s_bt_sel - 1;
        int dev_top = ui_list_top(dev_sel, count, UI_LIST_ROWS - 1);
        for (int i = 1; i < UI_LIST_ROWS; i++) {
            int idx = dev_top + (i - 1);
            const int sel = (dev_sel == idx);
            if (idx < count) {
                const char *nm = (idx < s_bt_list_cnt && idx < BT_MAX_DEVICES)
                                 ? s_bt_list_name[idx]
                                 : bluetooth_audio_device_name(idx);
                ui_list_row(&s_ui.bt_list, i, idx, count, nm, sel, false);
            }
            else {
                ui_list_row(&s_ui.bt_list, i, -1, 0, "", false, true);
            }
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
            ui_set_hint("↑↓选 A开关/连接 Select扫描 B返回");
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
            ui_set_hint(count ? "↑↓选 A开关/连接 Select扫描 B返回" : "Select扫描 B返回");
        }
}

/* Toggle the Bluetooth master switch. ON powers the radio up and scans; OFF
 * disconnects (bluetooth_audio_disable() tears the link down safely) and powers
 * the controller off. Persisted so the choice survives reboot. */
static void ui_bt_toggle_master(void)
{
    s_bt_on = !s_bt_on;
    if (s_bt_on) {
        bluetooth_audio_enable();
        bluetooth_audio_scan_start();
        set_action("蓝牙开");
    } else {
        bluetooth_audio_disable();
        set_action("蓝牙关");
        s_bt_sel = 0;   /* keep the highlight on the switch row */
    }
    nvs_handle_t h;
    if (nvs_open(UI_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, UI_NVS_BT, s_bt_on ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    ui_refresh();
}

void ui_bt_action(void)
{
        if (s_bt_sel == 0) {
            /* Master switch row: flip the radio on/off. */
            ui_bt_toggle_master();
            return;
        }
        /* Device row: real device index is (s_bt_sel - 1). */
        int dev_idx = s_bt_sel - 1;
        if (bluetooth_audio_is_connected()) {
            set_action("断开中");
            bluetooth_audio_disconnect();
            /* Return output to the speaker immediately (the route is explicit;
             * we don't wait for the link to actually drop). */
            hw_audio_set_route(AUDIO_ROUTE_SPEAKER);
        }
        else if (bluetooth_audio_device_count() > 0) {
            set_action("连接中");
            if (!bluetooth_audio_connect_index(dev_idx)) {
                set_action("连接失败");
            }
        }
        else {
            set_action("按Select扫描");
        }
}

void ui_bt_adjust(int step)
{
        if (!s_bt_on) {
            return;     /* only the switch row exists */
        }
        int count = bluetooth_audio_device_count();
        int total = 1 + count;
        s_bt_sel = (s_bt_sel - step + total) % total;
}

/* Select: start a fresh sink scan (only meaningful when the radio is on). */
void ui_bt_select(void)
{
    if (!s_bt_on) {
        return;
    }
    bluetooth_audio_scan_start();
    set_action("扫描中");
}

void ui_bt_reset_paint(void)
{
    /* The BT list is rebuilt on entry; nothing else to reset here. */
}
