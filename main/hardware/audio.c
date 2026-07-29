/*
 * Hardware layer: I2S audio output driving a MAX98357 mono Class-D DAC.
 * See hardware/audio.h.
 *
 * Wiring (from board_config.h):
 *   BCLK -> GPIO25, LRC(WS) -> GPIO32, DIN -> GPIO33, no MCLK.
 */
#include "board_config.h"
#include "hardware/audio.h"

#include <math.h>

#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "lvgl.h"

static const char *TAG = "hw_audio";

#define AUDIO_SAMPLE_RATE   32000
#define AUDIO_CHUNK         512
#define AUDIO_AMP_MAX       16000
#define AUDIO_2PI           6.2831853f

static i2s_chan_handle_t s_tx;
static bool s_ready;
static bool s_playing;
static uint32_t s_freq;
static uint32_t s_stop_at;
static float s_phase;
static uint8_t s_volume = 70;
static bool s_enabled = true;

void hw_audio_init(void)
{
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 4,
        .dma_frame_num = AUDIO_CHUNK,
        .auto_clear = true,
    };
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2S channel init failed: %s", esp_err_to_name(err));
        return;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                     I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_NUM_I2S_BCLK,
            .ws   = PIN_NUM_I2S_LRC,
            .dout = PIN_NUM_I2S_DIN,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2S std init failed: %s", esp_err_to_name(err));
        return;
    }
    err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2S channel enable failed: %s", esp_err_to_name(err));
        return;
    }
    s_ready = true;
}

bool hw_audio_ready(void)
{
    return s_ready;
}

void hw_audio_set_volume(uint8_t volume_pct)
{
    if (volume_pct > 100) {
        volume_pct = 100;
    }
    s_volume = volume_pct;
}

uint8_t hw_audio_get_volume(void)
{
    return s_volume;
}

void hw_audio_set_enabled(bool enabled)
{
    s_enabled = enabled;
}

bool hw_audio_is_enabled(void)
{
    return s_enabled;
}

void hw_audio_tone(uint32_t freq_hz, uint32_t duration_ms)
{
    if (!s_ready || !s_enabled) {
        return;
    }
    s_freq = freq_hz;
    s_phase = 0.0f;
    s_stop_at = lv_tick_get() + duration_ms;
    s_playing = true;
}

void hw_audio_process_timers(void)
{
    if (!s_playing || !s_ready) {
        return;
    }
    if ((int32_t)(lv_tick_get() - s_stop_at) >= 0) {
        s_playing = false;
        return;
    }

    int16_t buf[AUDIO_CHUNK];
    const int32_t amp = ((int32_t)AUDIO_AMP_MAX * (int32_t)s_volume) / 100;
    const float inc = (AUDIO_2PI * (float)s_freq) / (float)AUDIO_SAMPLE_RATE;
    for (int i = 0; i < AUDIO_CHUNK; i++) {
        buf[i] = (int16_t)((float)amp * sinf(s_phase));
        s_phase += inc;
        if (s_phase > AUDIO_2PI) {
            s_phase -= AUDIO_2PI;
        }
    }

    size_t written = 0;
    i2s_channel_write(s_tx, buf, sizeof(buf), &written, pdMS_TO_TICKS(50));
}
