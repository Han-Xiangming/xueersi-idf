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
#include "buttons.h"
#include "audio.h"
#include "battery.h"
#include "bt_audio.h"
#include "lcd.h"
#include "sd.h"
#include "lvgl.h"
#include "ui.h"
#include "player.h"
#include "ebook.h"
#include "rtc_wdt.h"
#include "esp_attr.h"
#include "hal/uart_ll.h"

/* LVGL UI refresh cadence in the main loop (milliseconds). */
#define UI_REFRESH_PERIOD_MS        16

static const char *TAG = "xiaomiao_dash";

/* --- Non-blocking console log sink --------------------------------------
 * The default console path (VFS -> uart driver) can BLOCK the caller for a
 * long time, and it must never be called from inside a vprintf hook (the
 * driver's internal ESP_RETURN_* / ESP_LOG* macros re-enter the log
 * subsystem -> recursive lock -> assert). This sink talks to the UART0
 * hardware FIFO DIRECTLY through the HAL LL layer: no driver, no esp_log,
 * no ISR, no blocking. If the TX FIFO does not drain (wedged UART,
 * disconnected USB-serial), the line is dropped after a short bounded
 * spin instead of hanging the caller. Logging can therefore never wedge a
 * task again, which is the mechanism behind the "logs freeze, only reboot
 * recovers, screen still alive" crashes. */
#define XM_LOG_BUF      512
#define XM_TX_SPIN_MAX  2000   /* ~µs-scale bounded spin per byte */

static int IRAM_ATTR xm_console_vprintf(const char *fmt, va_list ap)
{
    static char buf[XM_LOG_BUF];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n < 0) {
        return 0;
    }
    if (n > (int)sizeof(buf) - 1) {
        n = (int)sizeof(buf) - 1;
    }
    uart_dev_t *hw = UART_LL_GET_HW(UART_NUM_0);
    for (int i = 0; i < n; i++) {
        int spins = 0;
        while (uart_ll_get_txfifo_len(hw) == 0) {
            if (++spins >= XM_TX_SPIN_MAX) {
                return i;   /* TX wedged: drop the rest of the line */
            }
        }
        uart_ll_write_txfifo(hw, (const uint8_t *)&buf[i], 1);
    }
    return n;
}

/* --- Last-resort RTC watchdog -------------------------------------------
 * The task WDT and the interrupt WDT both need the CPU to actually run
 * interrupt handlers, so a total CPU freeze — a PSRAM bus stall while the
 * flash cache is disabled during a concurrent flash write, or an SDSPI
 * hardware hang — leaves the device dead until a manual power cycle (the
 * user-visible "logs stop, only reboot recovers" crash). The RTC watchdog
 * runs on its own RTC clock, independent of the CPU and its interrupts:
 * when fed stops for RTC_WDT_TIMEOUT_MS, it resets the system on its own.
 * The boot log then prints both CPUs' PCs at the moment of the reset,
 * which pinpoints the exact freeze site. The player watchdog task feeds it
 * every 2 s in every healthy state, so nothing normal can trip it. */
#define RTC_WDT_TIMEOUT_MS 30000

static void rtc_wdt_arm(void)
{
    rtc_wdt_protect_off();
    rtc_wdt_disable();
    rtc_wdt_set_length_of_reset_signal(RTC_WDT_SYS_RESET_SIG,
                                       RTC_WDT_LENGTH_3_2us);
    rtc_wdt_set_stage(RTC_WDT_STAGE0, RTC_WDT_STAGE_ACTION_RESET_SYSTEM);
    rtc_wdt_set_time(RTC_WDT_STAGE0, RTC_WDT_TIMEOUT_MS);
    rtc_wdt_enable();
    rtc_wdt_protect_on();
    ESP_LOGI(TAG, "RTC WDT armed (%u s system-reset backstop)",
             (unsigned)(RTC_WDT_TIMEOUT_MS / 1000));
}

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

/* Remote absolute volume (0..127, AVRCP full scale). Mapped to equal dB
 * steps inside the audio layer (see hw_audio_set_avrc_volume). */
static void xiaomiao_avrc_volume(uint8_t volume_0_127)
{
    hw_audio_set_avrc_volume(volume_0_127);
}

/* Low-battery guard (#9, improved): on the down-crossing, pause playback and
 * dim the backlight to at most 20 % (remembering the user's setting so it can
 * be restored on recovery). On the up-crossing, restore the saved backlight and
 * do NOT touch playback. Runs from the battery sample timer task; keep it short. */
static uint8_t s_bl_saved = 0;   /* user backlight captured at low-battery entry */

static void xiaomiao_battery_low(uint8_t pct, bool recovered)
{
    if (recovered) {
        if (s_bl_saved != 0) {
            hw_lcd_set_backlight(s_bl_saved);   /* restore user brightness */
            s_bl_saved = 0;
        }
        ESP_LOGI(TAG, "battery recovered %u%%, backlight restored", pct);
        return;
    }
    ESP_LOGW(TAG, "low battery %u%%, pausing playback / dimming backlight", pct);
    player_stop();
    s_bl_saved = hw_lcd_get_backlight();        /* capture before dimming */
    if (s_bl_saved > 20) {
        hw_lcd_set_backlight(20);               /* only dim if brighter than 20 */
    }
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

        /* Drive the auto screen-off (standby) idle timer each loop. */
        hw_lcd_standby_tick();

        usleep(delay_ms * 1000);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Xiaomiao LVGL 9.5 dashboard boot");

    /* Non-blocking console: a wedged UART must never freeze a task that
     * logs (see xm_console_vprintf). Installed before any subsystem logs. */
    esp_log_set_vprintf(xm_console_vprintf);

    /* Arm the last-resort RTC watchdog before anything that can stall the
     * CPU (see rtc_wdt_arm above). Fed every 2 s by the player watchdog
     * task once it starts. */
    rtc_wdt_arm();

    /* Bluedroid L2CAP floods "is_cong_cback_context" errors while the sink
     * is congested (known stack issue, espressif/esp-idf#7923, still present
     * in 5.5.4): every send attempt during the congestion callback logs one
     * ERROR. The flood is benign noise — drop the tag below ERROR. */
    esp_log_level_set("BT_L2CAP", ESP_LOG_WARN);

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
    hw_battery_init();
    hw_battery_set_low_warn(15, xiaomiao_battery_low);   /* #9 low-battery guard */
    bt_audio_init();
    bt_audio_set_avrc_cmd_cb(xiaomiao_avrc_cmd);
    bt_audio_set_avrc_volume_cb(xiaomiao_avrc_volume);
    hw_sd_try_mount();
    player_init();
    ebook_init();

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
