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
#include "freertos/queue.h"
#include "freertos/task.h"
#include "buttons.h"
#include "audio.h"
#include "battery.h"
#include "bluetooth_audio.h"
#include "lcd.h"
#include "sd.h"
#include "lvgl.h"
#include "ui.h"
#include "player.h"
#include "ebook.h"
#include "rtc_wdt.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "hal/uart_ll.h"

/* LVGL UI refresh cadence in the main loop (milliseconds). */
#define UI_REFRESH_PERIOD_MS        16
/* SD hotplug probe / mount-retry cadence (milliseconds). */
#define SD_HOTPLUG_POLL_MS          1000

static const char *TAG = "xiaomiao_dash";

/* AVRCP NEXT/PREV are delivered on the Bluedroid task. Queue them to the
 * LVGL task instead of calling player_next/prev() there: the player's index
 * bookkeeping is written by the UI/decode tasks, and this keeps all manual
 * navigation in one execution context (the existing list cursor auto-follow
 * in ui_pages_player.c then works without extra synchronisation). */
#define AVRC_NAV_QUEUE_LEN 4
static QueueHandle_t s_avrc_nav_q;

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
static portMUX_TYPE s_log_tx_mux = portMUX_INITIALIZER_UNLOCKED;

static int IRAM_ATTR xm_console_vprintf(const char *fmt, va_list ap)
{
    /* Stack-local, not static: esp_log_set_vprintf() is called from LVGL,
     * decode, BT and scan tasks concurrently. A shared static buffer let two
     * log lines interleave mid-format; a per-call buffer costs 512 bytes of
     * stack (safe on every task here) and needs no lock in this IRAM hook. */
    char buf[XM_LOG_BUF];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n < 0) {
        return 0;
    }
    if (n > (int)sizeof(buf) - 1) {
        n = (int)sizeof(buf) - 1;
    }
    /* Serialise the FIFO write so two tasks cannot interleave bytes from
     * different lines. The spin is bounded (XM_TX_SPIN_MAX), so the critical
     * section cannot hang; it only disables interrupts for a few µs per byte
     * on a healthy UART. TX-wedge path exits through the single exit below. */
    uart_dev_t *hw = UART_LL_GET_HW(UART_NUM_0);
    int sent = n;
    portENTER_CRITICAL_SAFE(&s_log_tx_mux);
    for (int i = 0; i < n; i++) {
        int spins = 0;
        while (uart_ll_get_txfifo_len(hw) == 0) {
            if (++spins >= XM_TX_SPIN_MAX) {
                sent = i;   /* TX wedged: drop the rest of the line */
                break;
            }
        }
        if (spins >= XM_TX_SPIN_MAX) {
            break;
        }
        uart_ll_write_txfifo(hw, (const uint8_t *)&buf[i], 1);
    }
    portEXIT_CRITICAL_SAFE(&s_log_tx_mux);
    return sent;
}

/* --- Last-resort RTC watchdog -------------------------------------------
 * The task WDT and the interrupt WDT both need the CPU to actually run
 * interrupt handlers, so a total CPU freeze — a PSRAM bus stall while the
 * flash cache is disabled during a concurrent flash write, or an SDSPI
 * hardware hang — leaves the device dead until a manual power cycle (the
 * user-visible "logs stop, only reboot recovers" crash). The RTC watchdog
 * runs on its own RTC clock, independent of the CPU and its interrupts:
 * when fed stops for RTC_WDT_TIMEOUT_MS, it resets the system on its own.
 * A pure hardware reset cannot capture the CPUs' PCs, so app_main prints
 * esp_reset_reason() and player_init prints the pre-reset player snapshot
 * kept in RTC_NOINIT memory; together they identify a silent freeze. The
 * player watchdog task feeds it every 2 s in every healthy state, so
 * nothing normal can trip it. */
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
    case BT_AVRC_CMD_PREV: {
        const int8_t dir = (cmd == BT_AVRC_CMD_NEXT) ? 1 : -1;
        if (s_avrc_nav_q == NULL) {
            ESP_LOGE(TAG, "AVRCP nav queue is NULL");
            break;
        }
        if (xQueueSend(s_avrc_nav_q, &dir, 0) != pdTRUE) {
            ESP_LOGW(TAG, "AVRCP %s dropped (nav queue full)",
                     dir > 0 ? "NEXT" : "PREV");
        }
        break;
    }
    }
}

/* Remote absolute volume (0..127, AVRCP full scale). Mapped to equal dB
 * steps inside the audio layer (see hw_audio_set_avrc_volume). */
static void xiaomiao_avrc_volume(uint8_t volume_0_127)
{
    hw_audio_set_avrc_volume(volume_0_127);
}

/* Local volume change on the Bluetooth route: mirror it to the connected
 * sink so the remote's own level tracks the device (AVRCP absolute volume,
 * Controller role). No-op when no sink is linked. */
static void xiaomiao_bt_volume_changed(uint8_t volume_pct)
{
    bluetooth_audio_set_sink_volume(volume_pct);
}

/* Current Bluetooth-route volume, pushed once when the AVRCP CT channel comes
 * up so the sink starts at the device's level instead of its own last one. */
static uint8_t xiaomiao_bt_volume_snapshot(void)
{
    return hw_audio_get_bt_volume();
}

/* SD hotplug teardown, run on the LVGL task from hw_sd_poll() just before
 * FATFS is unmounted. Order matters: save the reader position while the card
 * may still answer, then block new handle opens, then close the reader, then
 * wait for the player's decode/scan tasks. All UI-side ebook/player calls
 * happen on this same task, so no extra locking is needed. */
static void xiaomiao_sd_pre_unmount(void)
{
    ESP_LOGW(TAG, "SD hotplug: releasing card handles");
    ebook_progress_flush();
    ebook_sd_release();
    ebook_close();
    player_sd_release();
}

static void lvgl_task(void *arg)
{
    lv_group_t *group = (lv_group_t *)arg;
    uint32_t last_update_ms = 0;
    uint32_t last_sd_poll_ms = 0;
    bool was_standby = false;           /* edge detect for standby entry */

    ESP_LOGI(TAG, "Start Xiaomiao hardware dashboard");
    ui_create(group);
    lv_refr_now(NULL);
    for (uint8_t i = 0; i < 100 && !hw_lcd_first_flush_done(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    hw_lcd_display_on();

    while (true) {
        /* Drain AVRCP media-key navigation on the UI task (see queue above).
         * Idle presses are ignored, matching the player-page key handler. */
        int8_t nav;
        while (s_avrc_nav_q != NULL &&
               xQueueReceive(s_avrc_nav_q, &nav, 0) == pdTRUE) {
            if (player_state() == PLAYER_IDLE) {
                continue;
            }
            if (nav > 0) {
                player_next();
            }
            else {
                player_prev();
            }
        }

        /* SD hotplug: retry a missing card / probe a mounted one. This may
         * briefly block on SDSPI and, on card loss, runs the pre-unmount
         * callback that closes player/reader handles — hence on this task. */
        if (lv_tick_elaps(last_sd_poll_ms) >= SD_HOTPLUG_POLL_MS) {
            last_sd_poll_ms = lv_tick_get();
            hw_sd_poll();
        }

        if (lv_tick_elaps(last_update_ms) >= UI_REFRESH_PERIOD_MS) {
            last_update_ms = lv_tick_get();
            ui_refresh();
        }

        uint32_t delay_ms = lv_timer_handler();
        delay_ms = MAX(delay_ms, LVGL_TASK_MIN_DELAY_MS);
        delay_ms = MIN(delay_ms, LVGL_TASK_MAX_DELAY_MS);

        /* Drive the auto screen-off (standby) idle timer each loop. */
        hw_lcd_standby_tick();

        /* On entering standby the screen blanks: the user has stopped
         * interacting and may power the device off or close it, so flush the
         * reader position right away instead of relying on the 1.5 s debounce
         * window to survive a power drop. No-ops when no book is open, so it
         * is safe to call every standby edge. */
        const bool standby_now = hw_lcd_is_standby_active();
        if (standby_now && !was_standby) {
            ebook_progress_flush();
            /* Commit any debounced UI settings too. The playback gate inside
             * ui_settings_flush_force() still defers while audio is playing;
             * on entering standby the user has stopped interacting, so a
             * paused/idle session gets its settings flushed before power-off. */
            ui_settings_flush_force();
        }
        was_standby = standby_now;

        usleep(delay_ms * 1000);
    }
}

/* A silent freeze (RTC WDT, brownout, bus stall) leaves no runtime log, so
 * the reset reason is the only clue. Print it as the first line after the
 * non-blocking console is installed. */
static const char *reset_reason_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_UNKNOWN:   return "unknown";
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external pin";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic/exception";
    case ESP_RST_INT_WDT:   return "interrupt WDT";
    case ESP_RST_TASK_WDT:  return "task WDT";
    case ESP_RST_WDT:       return "WDT (RTC WDT backstop)";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT:  return "BROWNOUT (supply dipped)";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "other";
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Xiaomiao LVGL 9.5 dashboard boot");

    /* Non-blocking console: a wedged UART must never freeze a task that
     * logs (see xm_console_vprintf). Installed before any subsystem logs. */
    esp_log_set_vprintf(xm_console_vprintf);

    const esp_reset_reason_t rr = esp_reset_reason();
    ESP_LOGW(TAG, "[BOOT] reset reason: %s (%d)",
             reset_reason_name(rr), (int)rr);

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

    /* Must exist before any AVRCP callback can fire. */
    s_avrc_nav_q = xQueueCreate(AVRC_NAV_QUEUE_LEN, sizeof(int8_t));
    if (s_avrc_nav_q == NULL) {
        ESP_LOGE(TAG, "AVRCP nav queue creation failed");
    }

    hw_buttons_init();
    hw_lcd_init();
    hw_audio_init();
    hw_battery_init();
    bluetooth_audio_init();
    bluetooth_audio_set_avrc_cmd_cb(xiaomiao_avrc_cmd);
    bluetooth_audio_set_avrc_volume_cb(xiaomiao_avrc_volume);
    bluetooth_audio_set_avrc_sync_vol_cb(xiaomiao_bt_volume_snapshot);
    hw_audio_set_bt_volume_change_cb(xiaomiao_bt_volume_changed);
    hw_sd_set_pre_unmount_cb(xiaomiao_sd_pre_unmount);
    hw_sd_try_mount();
    player_init();
    /* Per ESP-IDF's watchdog guide, RTC_WDT must be explicitly fed or disabled
     * once kept enabled. Arm it only after its single feeder (mp3_watch) exists;
     * otherwise we would reboot every 30 s with no way to feed it. */
    if (player_watchdog_started()) {
        rtc_wdt_arm();
    } else {
        rtc_wdt_disable();
        ESP_LOGE(TAG, "[BOOT] mp3_watch missing; RTC WDT left disabled");
    }
    ebook_init();

    lv_init();
    lv_display_t *display = hw_lcd_create_display();
    lv_group_t *group = ui_input_init(display);
    ui_start_tick_timer();

    /* Pin the LVGL task to core 1: the Bluetooth controller + stack run on
     * core 0 by default, and an unpinned task can be scheduled there and
     * starved by BT's higher-priority work (visible as key-feedback jitter
     * while a sink is linked). Core 1 hosts the app alongside i2s_wr (6);
     * the decode task (8) is pinned to core 0, so a decode burst cannot
     * preempt key handling or the synchronous render here. */
    BaseType_t ret = xTaskCreatePinnedToCore(lvgl_task,
                                             "lvgl",
                                             LVGL_TASK_STACK_SIZE,
                                             group,
                                             LVGL_TASK_PRIORITY,
                                             NULL,
                                             1);
    ESP_ERROR_CHECK(ret == pdPASS ? ESP_OK : ESP_FAIL);
}
