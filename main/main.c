/*
 * Xiaomiao ESP32-WROVER-B LVGL 9.5 hardware pager.
 *
 * Application layer: boot the hardware drivers, create the LVGL display
 * and UI, then run the LVGL service loop. All peripheral logic lives in
 * the hardware/ drivers and all UI logic lives in app/ui; this file only wires
 * them together.
 */

#include <unistd.h>
#include <sys/param.h>

#include "board_config.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hardware/buttons.h"
#include "hardware/audio.h"
#include "hardware/bt_audio.h"
#include "hardware/lcd.h"
#include "hardware/sd.h"
#include "lvgl.h"
#include "app/ui.h"
#include "app/player.h"

/* LVGL UI refresh cadence in the main loop (milliseconds). */
#define UI_REFRESH_PERIOD_MS        16

static const char *TAG = "xiaomiao_dash";

/* Remote AVRCP command from a paired headset/speaker drives local playback. */
static void xiaomiao_avrc_cmd(bt_avrc_cmd_t cmd)
{
    switch (cmd) {
    case BT_AVRC_CMD_PLAY:
        if (player_state() == PLAYER_PAUSED) {
            player_toggle();                   /* resume a paused track */
        }
        break;
    case BT_AVRC_CMD_PAUSE:
        if (player_state() == PLAYER_PLAYING) {
            player_toggle();                   /* pause a playing track */
        }
        break;
    case BT_AVRC_CMD_STOP:
        player_stop();
        break;
    case BT_AVRC_CMD_NEXT:
    case BT_AVRC_CMD_PREV:
        /* The current player has no playlist navigation; ignore for now. */
        ESP_LOGI(TAG, "AVRCP %s (no playlist navigation)",
                 cmd == BT_AVRC_CMD_NEXT ? "NEXT" : "PREV");
        break;
    }
}

/* Remote absolute volume (0..127, AVRCP full scale) -> 0..100% master volume. */
static void xiaomiao_avrc_volume(uint8_t volume_0_127)
{
    uint8_t pct = (uint8_t)(((uint32_t)volume_0_127 * 100u + 63) / 127u);
    hw_audio_set_volume(pct);
}

static void lvgl_task(void *arg)
{
    lv_group_t *group = (lv_group_t *)arg;
    uint32_t last_update_ms = 0;

    ESP_LOGI(TAG, "Start Xiaomiao hardware dashboard");
    ui_create(group);
    lv_refr_now(NULL);
    for (uint8_t i = 0; i < 100 && !hw_lcd_first_flush_done(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    hw_lcd_display_on();

    while (true) {
        if (lv_tick_elaps(last_update_ms) >= UI_REFRESH_PERIOD_MS) {
            last_update_ms = lv_tick_get();
            ui_refresh();
        }

        uint32_t delay_ms = lv_timer_handler();
        delay_ms = MAX(delay_ms, LVGL_TASK_MIN_DELAY_MS);
        delay_ms = MIN(delay_ms, LVGL_TASK_MAX_DELAY_MS);
        usleep(delay_ms * 1000);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Xiaomiao LVGL 9.5 dashboard boot");

    /* NVS must be up before any Wi-Fi/BT controller init (those trigger
     * phy_init, which loads RF calibration data from NVS). Handle the
     * "flash needs re-format" cases by erasing and re-initialising. */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    hw_buttons_init();
    hw_lcd_init();
    hw_audio_init();
    bt_audio_init();
    bt_audio_set_avrc_cmd_cb(xiaomiao_avrc_cmd);
    bt_audio_set_avrc_volume_cb(xiaomiao_avrc_volume);
    hw_sd_try_mount();
    player_init();

    lv_init();
    lv_display_t *display = hw_lcd_create_display();
    lv_group_t *group = ui_input_init(display);
    ui_start_tick_timer();

    BaseType_t ret = xTaskCreate(lvgl_task,
                                 "lvgl",
                                 LVGL_TASK_STACK_SIZE,
                                 group,
                                 LVGL_TASK_PRIORITY,
                                 NULL);
    ESP_ERROR_CHECK(ret == pdPASS ? ESP_OK : ESP_FAIL);
}
