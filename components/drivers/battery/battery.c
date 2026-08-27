/*
 * Hardware layer: single-cell Li-ion battery level sensing.
 *
 * Wiring: V_bat --[100k]-- GPIO39 --[100k]-- GND  (two 0.1 MΩ "01D" parts).
 * GPIO 39 is ADC1 channel 3. The 1/2 divider keeps the ADC pin within the
 * 0..3.3 V range for pack voltages up to ~6.6 V (covers a 4.2 V Li-ion cell).
 *
 * ADC reading uses the ESP-IDF "oneshot" driver (noise-free single sample)
 * with the line-fitting calibration scheme, which uses the chip's eFuse
 * reference when present and falls back to a default VREF otherwise. A small
 * sliding window smooths the reading.
 */
#include "board_config.h"
#include "battery.h"

#include <math.h>

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "audio.h"   /* hw_audio_is_playing / hw_audio_get_volume */

static const char *TAG = "hw_battery";

/* ADC setup. */
#define BAT_ADC_UNIT            ADC_UNIT_1
#define BAT_ADC_CH              ADC_CHANNEL_3   /* GPIO 39 */
#define BAT_ADC_ATTEN           ADC_ATTEN_DB_12 /* 0..3.3 V span, max range */
#define BAT_ADC_WIDTH           ADC_BITWIDTH_12

/* ADC full-scale voltage (mV) under the chosen attenuation, used only by the
 * no-calibration fallback. DB_12 → 3.3 V; keep in sync with BAT_ADC_ATTEN. */
#define BAT_ADC_VMAX_MV         3300

/* Sampling / smoothing. */
#define BAT_SAMPLE_PERIOD_MS    250             /* poll 4x per second for faster averaging */
#define BAT_WINDOW_SIZE         16              /* sliding-average depth (~4 s window) */
#define BAT_INVALID_PERCENT     0
#define BAT_DEFAULT_VREF_MV     3300            /* used if eFuse not burnt */

/* Hysteresis on the displayed percentage: ignore sub-step jitter so the UI
 * does not flicker between adjacent values. */
#define BAT_PCT_HYSTERESIS      1               /* % change below this is ignored */

/* Sliding window of calibrated pin voltages (mV). */
static int      s_window[BAT_WINDOW_SIZE];
static uint8_t  s_window_idx;
static uint8_t  s_window_filled;

/* ADC driver handles (persist for the timer callback). */
static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t         s_cali_handle = NULL;

/* Cached, converted results. */
static float    s_voltage = 0.0f;
static uint8_t  s_percent = BAT_INVALID_PERCENT;   /* raw mapped value */
static uint8_t  s_percent_disp = BAT_INVALID_PERCENT; /* hysteresis-filtered for display */
static bool     s_inited  = false;

/* I2S load handling. */
/* Per-100%-volume load-compensation slope, in mV added to the pack voltage.
 * The applied offset = comp_mv_per_100pct * volume_pct / 100. Default ~50 mV
 * at full volume; tune to your cell's sag under the MAX98357 load. */
static uint16_t s_load_comp_mv = 50;
/* Hold the last rested reading while audio is actively playing, so the gauge
 * does not creep down under load. Forced samples still refresh. */
static bool     s_freeze_while_playing = true;

/* Map a pack voltage to 0..100 % via a piecewise open-circuit (rested) Li-ion
 * table. A single-cell Li-ion curve is strongly non-linear: it sits on a long
 * ~3.7 V plateau, so a linear 3.30→4.20 V map would report ~50 % near 3.75 V
 * (actually ~70 %) and make the gauge jitter. The table below is entered as
 * (voltage_V, percent) breakpoints, sorted ascending; values between breakpoints
 * are linearly interpolated, and clamped to 0/100 outside the ends.
 *
 * Calibrated for a typical 4.20 V-charged single Li-ion cell at rest (no load).
 * Under audio-playback load the live voltage sags a few tens of mV, so treat the
 * result as a coarse gauge, not an exact fuel figure. */
typedef struct {
    float v;   /* pack voltage, volts */
    uint8_t p; /* corresponding state-of-charge, % */
} bat_lut_t;

static const bat_lut_t s_bat_lut[] = {
    {3.30f,   0},   /* cut-off */
    {3.45f,  10},
    {3.55f,  20},
    {3.62f,  30},
    {3.68f,  40},
    {3.72f,  50},
    {3.78f,  60},
    {3.85f,  70},
    {3.93f,  80},
    {4.02f,  90},
    {4.20f, 100},   /* full charge */
};

/* Map a pack voltage to 0..100 % via the open-circuit table above. */
static uint8_t voltage_to_percent(float vbat)
{
    const size_t n = sizeof(s_bat_lut) / sizeof(s_bat_lut[0]);
    if (vbat <= s_bat_lut[0].v) {
        return s_bat_lut[0].p;
    }
    if (vbat >= s_bat_lut[n - 1].v) {
        return s_bat_lut[n - 1].p;
    }
    for (size_t i = 1; i < n; i++) {
        if (vbat <= s_bat_lut[i].v) {
            const float v0 = s_bat_lut[i - 1].v;
            const float v1 = s_bat_lut[i].v;
            const int p0 = s_bat_lut[i - 1].p;
            const int p1 = s_bat_lut[i].p;
            const int pct = p0 + (int)((float)(p1 - p0) * (vbat - v0) / (v1 - v0) + 0.5f);
            return (pct < 0) ? 0 : (pct > 100 ? 100 : (uint8_t)pct);
        }
    }
    return 100;
}

static int window_average(void)
{
    if (s_window_filled == 0) {
        return 0;
    }
    int sum = 0;
    for (uint8_t i = 0; i < s_window_filled; ++i) {
        sum += s_window[i];
    }
    return sum / s_window_filled;
}

/* Internal sampler. `forced` bypasses the freeze-while-playing guard so a
 * settings-page refresh still updates the gauge mid-playback. */
static void battery_sample_int(bool forced)
{
    if (!s_inited) {
        return;
    }

    /* Freeze guard (方案2): while audio is actively clocking to the DAC, hold
     * the last displayed reading instead of letting the load sag drag it down.
     * The ADC is still read below (keeps the window fresh), but we skip the
     * conversion/display update unless this is a forced sample. */
    const bool playing = hw_audio_is_playing();
    if (!forced && s_freeze_while_playing && playing) {
        return;
    }

    int raw = 0;
    esp_err_t err = adc_oneshot_read(s_adc_handle, BAT_ADC_CH, &raw);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "adc_oneshot_read failed: %s", esp_err_to_name(err));
        return;
    }

    int v_pin_mv = 0;
    if (s_cali_handle != NULL) {
        err = adc_cali_raw_to_voltage(s_cali_handle, raw, &v_pin_mv);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "adc_cali_raw_to_voltage failed: %s", esp_err_to_name(err));
            return;
        }
    }
    else {
        /* Fallback: assume a linear 0..VMAX map over the 12-bit span. */
        if (raw < 0) {
            raw = 0;
        }
        if (raw > (1 << 12)) {
            raw = (1 << 12);
        }
        v_pin_mv = (int)(((float)raw / (float)(1 << 12)) * BAT_ADC_VMAX_MV);
    }

    s_window[s_window_idx] = v_pin_mv;
    s_window_idx = (s_window_idx + 1) % BAT_WINDOW_SIZE;
    if (s_window_filled < BAT_WINDOW_SIZE) {
        s_window_filled++;
    }

    const float v_pin_v = ((float)window_average()) / 1000.0f;
    float vbat = v_pin_v * BAT_DIV_FACTOR;

    /* Load compensation (方案1): the pack sags under speaker load, so add an
     * offset that scales with the current volume. Only meaningful while
     * playing; when idle the rested voltage is already correct. */
    if (playing) {
        const float comp_mv = (float)s_load_comp_mv *
                              (float)hw_audio_get_volume() / 100.0f;
        vbat += comp_mv / 1000.0f;
    }

    s_voltage = vbat;
    s_percent = voltage_to_percent(s_voltage);

    /* Apply display hysteresis: only move the shown value when the raw
     * percentage changes by more than BAT_PCT_HYSTERESIS, which suppresses
     * flicker from ADC noise around a steady state. */
    if (s_percent_disp == BAT_INVALID_PERCENT) {
        s_percent_disp = s_percent;
    }
    else if (s_percent >= s_percent_disp) {
        if (s_percent - s_percent_disp > BAT_PCT_HYSTERESIS) {
            s_percent_disp = s_percent;
        }
    }
    else {
        if (s_percent_disp - s_percent > BAT_PCT_HYSTERESIS) {
            s_percent_disp = s_percent;
        }
    }

    ESP_LOGD(TAG, "raw=%d v_pin=%d mV vbat=%.2f V pct=%u%% (%s)",
             raw, v_pin_mv, s_voltage, s_percent, playing ? "playing" : "idle");
}

void hw_battery_sample(void)
{
    battery_sample_int(false);
}

void hw_battery_set_load_comp_mv(uint16_t comp_mv_per_100pct)
{
    s_load_comp_mv = comp_mv_per_100pct;
}

uint16_t hw_battery_get_load_comp_mv(void)
{
    return s_load_comp_mv;
}

void hw_battery_set_freeze_while_playing(bool enable)
{
    s_freeze_while_playing = enable;
}

bool hw_battery_get_freeze_while_playing(void)
{
    return s_freeze_while_playing;
}

static void battery_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    hw_battery_sample();
}

void hw_battery_init(void)
{
    if (s_inited) {
        return;
    }

    /* ADC oneshot unit (ADC1). Leave clk_src at its default. */
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = BAT_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed: %s", esp_err_to_name(err));
        return;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = BAT_ADC_ATTEN,
        .bitwidth = BAT_ADC_WIDTH,
    };
    err = adc_oneshot_config_channel(s_adc_handle, BAT_ADC_CH, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_config_channel failed: %s", esp_err_to_name(err));
        adc_oneshot_del_unit(s_adc_handle);
        s_adc_handle = NULL;
        return;
    }

    /* Line-fitting calibration: uses eFuse VREF if present, else the default. */
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id = BAT_ADC_UNIT,
        .atten = BAT_ADC_ATTEN,
        .bitwidth = BAT_ADC_WIDTH,
#ifdef CONFIG_IDF_TARGET_ESP32
        .default_vref = BAT_DEFAULT_VREF_MV,
#endif
    };
    if (adc_cali_create_scheme_line_fitting(&cali_cfg, &s_cali_handle) == ESP_OK) {
        ESP_LOGI(TAG, "ADC line-fitting calibration enabled");
    }
    else
#endif
    {
        ESP_LOGW(TAG, "ADC calibration unavailable; using nominal VREF");
        s_cali_handle = NULL;
    }

    s_window_idx = 0;
    s_window_filled = 0;
    s_voltage = 0.0f;
    s_percent = BAT_INVALID_PERCENT;
    s_percent_disp = BAT_INVALID_PERCENT;
    s_inited = true;

    /* Prime the window with one immediate sample, then poll periodically. */
    hw_battery_sample();

    TimerHandle_t timer = xTimerCreate("bat_sample",
                                       pdMS_TO_TICKS(BAT_SAMPLE_PERIOD_MS),
                                       pdTRUE,   /* auto-reload */
                                       NULL,
                                       battery_timer_cb);
    if (timer != NULL) {
        if (xTimerStart(timer, pdMS_TO_TICKS(100)) != pdPASS) {
            ESP_LOGW(TAG, "battery sample timer failed to start");
        }
    }
    else {
        ESP_LOGW(TAG, "battery sample timer alloc failed");
    }

    ESP_LOGI(TAG, "battery sense on GPIO%d (ADC1_CH%d), divider %.2f",
             PIN_NUM_BAT_ADC, BAT_ADC_CH, BAT_DIV_FACTOR);
}

float hw_battery_voltage(void)
{
    return s_voltage;
}

uint8_t hw_battery_percent(void)
{
    return s_percent_disp;
}


