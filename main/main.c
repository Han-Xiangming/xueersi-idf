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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hardware/buttons.h"
#include "hardware/audio.h"
#include "hardware/bt_audio.h"
#include "hardware/lcd.h"
#include "hardware/sd.h"
#include "hardware/wifi_prov.h"
#include "lvgl.h"
#include "app/ui.h"
#include "app/player.h"

/* LVGL UI refresh cadence in the main loop (milliseconds). */
#define UI_REFRESH_PERIOD_MS        16

static const char *TAG = "xiaomiao_dash";

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

    /* LCD is fully up. Defer Wi-Fi bring-up until now: the first full-screen
     * SPI flush allocates a ~40 KB internal-DRAM priv TX buffer, and we must
     * not let esp_wifi_start()'s ~54 KB of dynamic RX/TX buffers race it for
     * that last chunk of internal DRAM (the priv buffer is then cached and
     * reused for all later flushes). */
    wifi_prov_start();

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

    hw_buttons_init();
    hw_lcd_init();
    hw_audio_init();
    wifi_prov_init();   /* Wi-Fi + NVS before BT (controller coex) */
    bt_audio_init();
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
