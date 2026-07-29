/*
 * Hardware layer: passive buzzer driven by LEDC PWM.
 * See hardware/buzzer.h.
 */
#include "board_config.h"
#include "hardware/buzzer.h"

#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "hw_buzzer";

#define BUZZER_LEDC_MODE            LEDC_LOW_SPEED_MODE
#define BUZZER_LEDC_TIMER           LEDC_TIMER_0
#define BUZZER_LEDC_CHANNEL         LEDC_CHANNEL_0
#define BUZZER_DUTY                 128

static bool s_buzzer_ready;
static uint32_t s_buzzer_stop_at;

static void buzzer_stop(void)
{
    if (!s_buzzer_ready) {
        return;
    }
    ledc_stop(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, 0);
    s_buzzer_stop_at = 0;
}

void hw_buzzer_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode = BUZZER_LEDC_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = BUZZER_LEDC_TIMER,
        .freq_hz = 880,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Buzzer timer init failed: %s", esp_err_to_name(err));
        return;
    }

    ledc_channel_config_t channel_cfg = {
        .gpio_num = PIN_NUM_BUZZER,
        .speed_mode = BUZZER_LEDC_MODE,
        .channel = BUZZER_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BUZZER_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
    };
    err = ledc_channel_config(&channel_cfg);
    if (err == ESP_OK) {
        s_buzzer_ready = true;
    }
    else {
        ESP_LOGW(TAG, "Buzzer channel init failed: %s", esp_err_to_name(err));
    }
}

void hw_buzzer_beep(uint32_t freq_hz, uint32_t ms)
{
    if (!s_buzzer_ready) {
        return;
    }
    esp_err_t err = ledc_set_freq(BUZZER_LEDC_MODE, BUZZER_LEDC_TIMER, freq_hz);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Buzzer frequency %lu Hz failed: %s", (unsigned long)freq_hz, esp_err_to_name(err));
        return;
    }
    err = ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, BUZZER_DUTY);
    if (err == ESP_OK) {
        err = ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Buzzer duty update failed: %s", esp_err_to_name(err));
        return;
    }
    s_buzzer_stop_at = lv_tick_get() + ms;
}

void hw_buzzer_stop(void)
{
    buzzer_stop();
}

bool hw_buzzer_ready(void)
{
    return s_buzzer_ready;
}

void hw_buzzer_process_timers(void)
{
    uint32_t now = lv_tick_get();

    if (s_buzzer_stop_at && (int32_t)(now - s_buzzer_stop_at) >= 0) {
        buzzer_stop();
    }
}
