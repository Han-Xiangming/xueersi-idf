/*
 * Hardware layer: ST7789 LCD driver and LVGL display binding.
 * See lcd.h.
 */
#include <assert.h>
#include <sys/param.h>

#include "board_config.h"
#include "lcd.h"

#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "hw_lcd";

/* ST7789 command set */
#define ST7789_SWRESET              0x01
#define ST7789_SLPOUT               0x11
#define ST7789_NORON                0x13
#define ST7789_INVOFF               0x20
#define ST7789_INVON                0x21
#define ST7789_DISPOFF              0x28
#define ST7789_DISPON               0x29
#define ST7789_CASET                0x2A
#define ST7789_RASET                0x2B
#define ST7789_RAMWR                0x2C
#define ST7789_MADCTL               0x36
#define ST7789_COLMOD               0x3A

#define MADCTL_MY                   0x80
#define MADCTL_MX                   0x40
#define MADCTL_MV                   0x20
#define MADCTL_RGB                  0x00

static esp_lcd_panel_io_handle_t s_lcd_io_handle;
static bool s_lcd_display_on;
static volatile bool s_lcd_first_flush_done;

/* Backlight PWM (LEDC) on PIN_NUM_LCD_BL. 0..100 % maps to 0..duty_max. */
#define BL_LEDC_TIMER         LEDC_TIMER_1
#define BL_LEDC_CHANNEL       LEDC_CHANNEL_1
#define BL_LEDC_SPEED_HZ      5000
#define BL_LEDC_DUTY_RES      LEDC_TIMER_10_BIT   /* 0..1023 */
#define BL_DUTY_MAX           ((1 << 10) - 1)
static bool s_bl_inited;
static uint8_t s_bl_percent = 100;   /* last set brightness, for hw_lcd_get_backlight */

/* Guards lv_display_flush_ready so it is signalled exactly once per flush:
 * on the success path it is signalled from the asynchronous DMA tx-done
 * callback (lcd_flush_ready_cb), while on the error path (where no DMA
 * transfer completes) it is signalled inline. Without this guard the
 * success path would call it twice — once here and again from the tx-done
 * callback — which corrupts LVGL's flush state and can deadlock rendering. */
static bool s_flush_ready_pending = false;

/* ---- Auto screen-off (standby) ----
 * Idle timer: after s_standby_timeout_ms of no activity the backlight is
 * switched off and the panel is put into DISPOFF to save power. Any call to
 * hw_lcd_activity() (wired to key presses in main.c) wakes it back up. */
static bool     s_standby_enabled   = false;   /* feature on/off */
static uint32_t s_standby_timeout_ms = 30000;  /* idle delay before standby */
static int64_t  s_last_activity_ms  = 0;       /* timestamp of last activity */
static bool     s_standby_active    = false;   /* currently blanked */

static void st7789_tx_param(esp_lcd_panel_io_handle_t io_handle, int cmd, const void *param, size_t param_size)
{
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, cmd, param, param_size));
}

static void st7789_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void st7789_clear_black(esp_lcd_panel_io_handle_t io_handle)
{
    static uint16_t line[LCD_H_RES * 8];
    const uint8_t caset[] = {
        0x00, 0x00,
        (LCD_H_RES - 1) >> 8, (LCD_H_RES - 1) & 0xFF,
    };

    memset(line, 0, sizeof(line));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, ST7789_CASET, caset, sizeof(caset)));
    for (uint16_t y = 0; y < LCD_V_RES; y += 8) {
        const uint16_t y2 = MIN((uint16_t)(y + 7), (uint16_t)(LCD_V_RES - 1));
        const uint8_t raset[] = {
            y >> 8, y & 0xFF,
            y2 >> 8, y2 & 0xFF,
        };
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, ST7789_RASET, raset, sizeof(raset)));
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(io_handle, ST7789_RAMWR, line, (y2 - y + 1) * LCD_H_RES * sizeof(uint16_t)));
    }
}

static void st7789_init_240x320_rot90(esp_lcd_panel_io_handle_t io_handle)
{
    const uint8_t colmod[] = {0x55}; /* RGB565 */
    const uint8_t madctl_default[] = {MADCTL_RGB};
    const uint8_t caset[] = {
        0x00, 0x00,
        (LCD_NATIVE_H_RES - 1) >> 8, (LCD_NATIVE_H_RES - 1) & 0xFF,
    };
    const uint8_t raset[] = {
        0x00, 0x00,
        (LCD_NATIVE_V_RES - 1) >> 8, (LCD_NATIVE_V_RES - 1) & 0xFF,
    };
    const uint8_t madctl_rot90[] = {MADCTL_MV | MADCTL_MY | MADCTL_RGB};

    ESP_LOGI(TAG, "Initialize ST7789 panel (native 240x320, rotated 90 deg)");
    st7789_tx_param(io_handle, ST7789_DISPOFF, NULL, 0);
    st7789_tx_param(io_handle, ST7789_SWRESET, NULL, 0);
    st7789_delay_ms(150);
    st7789_tx_param(io_handle, ST7789_SLPOUT, NULL, 0);
    st7789_delay_ms(500);
    st7789_tx_param(io_handle, ST7789_MADCTL, madctl_default, sizeof(madctl_default));
    st7789_tx_param(io_handle, ST7789_COLMOD, colmod, sizeof(colmod));
    st7789_delay_ms(10);
    st7789_tx_param(io_handle, ST7789_CASET, caset, sizeof(caset));
    st7789_tx_param(io_handle, ST7789_RASET, raset, sizeof(raset));
    st7789_tx_param(io_handle, ST7789_INVON, NULL, 0);
    st7789_tx_param(io_handle, ST7789_NORON, NULL, 0);
    st7789_delay_ms(10);
    st7789_tx_param(io_handle, ST7789_MADCTL, madctl_rot90, sizeof(madctl_rot90));
    st7789_clear_black(io_handle);
}

static void lcd_display_on(void)
{
    if (s_lcd_display_on || !s_lcd_io_handle) {
        return;
    }

    st7789_tx_param(s_lcd_io_handle, ST7789_DISPON, NULL, 0);
    st7789_delay_ms(20);
    s_lcd_display_on = true;
}

static bool lcd_flush_ready_cb(esp_lcd_panel_io_handle_t panel_io,
                               esp_lcd_panel_io_event_data_t *edata,
                               void *user_ctx)
{
    (void)panel_io;
    (void)edata;

    lv_display_t *display = (lv_display_t *)user_ctx;
    if (s_flush_ready_pending) {
        s_flush_ready_pending = false;
        s_lcd_first_flush_done = true;
        lv_display_flush_ready(display);
    }
    return false;
}

static void lvgl_flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_io_handle_t io_handle = lv_display_get_user_data(display);
    const int width = area->x2 - area->x1 + 1;
    /* The flush is now in flight. It will be signalled "ready" exactly once:
     * either from lcd_flush_ready_cb when the final async tx_color DMA
     * completes (success path), or inline below on a transfer error (where
     * no DMA completes and the callback will never fire). */
    s_flush_ready_pending = true;
    const size_t line_bytes = (size_t)width * sizeof(uint16_t);
    const uint8_t *line = px_map;

    /* Send the dirty rectangle one row at a time instead of as a single big
     * block. esp_lcd_panel_io_tx_color() allocates a private DMA "TX buffer"
     * when the payload is large / not DMA-friendly, and a full-screen flush
     * (320*240*2 = 153600 bytes) can fail that allocation (ESP_ERR_NO_MEM ->
     * abort). Per-row transfers cap the payload at width*2 bytes (<= 640 for
     * this panel), so the internal buffer is always tiny and the allocation
     * succeeds even under PSRAM fragmentation. */
    for (int y = area->y1; y <= area->y2; ++y) {
        const uint16_t x_start = area->x1 + LCD_X_GAP;
        const uint16_t x_end = area->x2 + LCD_X_GAP;
        const uint16_t y_pos = y + LCD_Y_GAP;
        const uint8_t caset[] = {
            x_start >> 8, x_start & 0xFF,
            x_end >> 8, x_end & 0xFF,
        };
        const uint8_t raset[] = {
            y_pos >> 8, y_pos & 0xFF,
            y_pos >> 8, y_pos & 0xFF,
        };

        esp_err_t err = esp_lcd_panel_io_tx_param(io_handle, ST7789_CASET, caset, sizeof(caset));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "flush caset failed: %s", esp_err_to_name(err));
            /* No DMA will complete for this frame, so signal ready inline.
             * The guard ensures it happens at most once. */
            if (s_flush_ready_pending) {
                s_flush_ready_pending = false;
                lv_display_flush_ready(display);
            }
            return;
        }
        err = esp_lcd_panel_io_tx_param(io_handle, ST7789_RASET, raset, sizeof(raset));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "flush raset failed: %s", esp_err_to_name(err));
            if (s_flush_ready_pending) {
                s_flush_ready_pending = false;
                lv_display_flush_ready(display);
            }
            return;
        }
        err = esp_lcd_panel_io_tx_color(io_handle, ST7789_RAMWR, line, line_bytes);
        if (err != ESP_OK) {
            /* Never abort on a transient transfer error: just unlock the
             * display so LVGL can retry next frame instead of deadlocking or
             * crashing the whole firmware. No DMA completes here either, so
             * signal ready inline (guarded against a duplicate call). */
            ESP_LOGW(TAG, "flush line %d failed: %s", y, esp_err_to_name(err));
            if (s_flush_ready_pending) {
                s_flush_ready_pending = false;
                lv_display_flush_ready(display);
            }
            return;
        }
        line += line_bytes;
    }

    /* Success path: the last tx_color above is asynchronous (DMA). Its
     * completion triggers lcd_flush_ready_cb, which will signal LVGL exactly
     * once via the s_flush_ready_pending guard. Do NOT call
     * lv_display_flush_ready() here — doing so would signal it twice. */
}

/* Forward declaration so hw_lcd_init() can call it before its definition. */
static void backlight_init(void);

void hw_lcd_init(void)
{
    ESP_LOGI(TAG, "Initialize SPI bus for ST7789 TFT");
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_NUM_LCD_SCLK,
        .mosi_io_num = PIN_NUM_LCD_MOSI,
        .miso_io_num = PIN_NUM_LCD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_DRAW_BUF_LINES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_NUM_LCD_DC,
        .cs_gpio_num = PIN_NUM_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                             &io_config,
                                             &io_handle));
    s_lcd_io_handle = io_handle;
    s_lcd_display_on = false;
    s_lcd_first_flush_done = false;
    st7789_init_240x320_rot90(io_handle);

    backlight_init();   /* default PWM at full brightness until UI overrides */
}

/* Partial-refresh draw buffers sized for LCD_DRAW_BUF_LINES lines. Prefer
 * external PSRAM (DMA-capable); fall back to the internal DMA pool if PSRAM
 * is not present. */
static void *alloc_draw_buf(size_t sz)
{
    void *p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (p == NULL) {
        p = spi_bus_dma_memory_alloc(LCD_HOST, sz, 0);
    }
    return p;
}

lv_display_t *hw_lcd_create_display(void)
{
    lv_display_t *display = lv_display_create(LCD_H_RES, LCD_V_RES);
    assert(display);

    const lv_color_format_t color_format = LV_COLOR_FORMAT_RGB565_SWAPPED;
    const uint32_t stride = lv_draw_buf_width_to_stride(LCD_H_RES, color_format);
    const size_t draw_buffer_sz = stride * LCD_DRAW_BUF_LINES;
    /* Partial rendering: the draw buffers cover only LCD_DRAW_BUF_LINES lines,
     * so LVGL refreshes just the dirty area of the panel. Allocate from
     * external PSRAM (DMA-capable) first; fall back to the internal DMA pool
     * if PSRAM is unavailable. Using only spi_bus_dma_memory_alloc() draws
     * from the very small internal DMA pool and allocations fail on boot. */
    void *buf1 = alloc_draw_buf(draw_buffer_sz);
    void *buf2 = alloc_draw_buf(draw_buffer_sz);
    assert(buf1);
    assert(buf2);

    lv_display_set_color_format(display, color_format);
    lv_display_set_dpi(display, LCD_DPI);
    lv_display_set_buffers(display, buf1, buf2, draw_buffer_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_user_data(display, s_lcd_io_handle);
    lv_display_set_flush_cb(display, lvgl_flush_cb);

    esp_lcd_panel_io_callbacks_t callbacks = {
        .on_color_trans_done = lcd_flush_ready_cb,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(s_lcd_io_handle, &callbacks, display));

    ESP_LOGI(TAG,
             "LVGL display: %dx%d, dpi=%d, %d partial-refresh DMA buffers of %d lines, SPI=%d MHz",
             LCD_H_RES,
             LCD_V_RES,
             LCD_DPI,
             LCD_DRAW_BUF_COUNT,
             LCD_DRAW_BUF_LINES,
             LCD_PIXEL_CLOCK_HZ / 1000000);

    return display;
}

void hw_lcd_display_on(void)
{
    lcd_display_on();
}

/* Configure LEDC timer + channel to drive the backlight pin with a 5 kHz PWM.
 * Active-high: duty 0 = off, BL_DUTY_MAX = full brightness. */
static void backlight_init(void)
{
    if (s_bl_inited) {
        return;
    }

    const ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BL_LEDC_DUTY_RES,
        .timer_num       = BL_LEDC_TIMER,
        .freq_hz         = BL_LEDC_SPEED_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    const ledc_channel_config_t chan_cfg = {
        .gpio_num   = PIN_NUM_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = BL_LEDC_CHANNEL,
        .timer_sel  = BL_LEDC_TIMER,
        .duty       = BL_DUTY_MAX,   /* default: full brightness */
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&chan_cfg));
    s_bl_inited = true;
}

/* Set backlight brightness as a percentage (0..100). Clamped to range. */
void hw_lcd_set_backlight(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    if (!s_bl_inited) {
        backlight_init();
    }
    s_bl_percent = percent;            /* remember for hw_lcd_get_backlight */
    const uint32_t duty = (BL_DUTY_MAX * percent) / 100;
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL));
}

uint8_t hw_lcd_get_backlight(void)
{
    return s_bl_inited ? s_bl_percent : 0;
}

bool hw_lcd_first_flush_done(void)
{
    return s_lcd_first_flush_done;
}

/* ---- Auto screen-off (standby) ---- */

/* Enable/disable the auto screen-off feature and set the idle timeout.
 * timeout_ms == 0 disables standby (screen stays on permanently). */
void hw_lcd_set_standby_timeout(uint32_t timeout_ms)
{
    s_standby_enabled = (timeout_ms > 0);
    s_standby_timeout_ms = timeout_ms;
    if (s_standby_enabled) {
        /* Re-arm the idle timer so the timeout applies from now. */
        s_last_activity_ms = esp_timer_get_time() / 1000;
    } else if (s_standby_active) {
        /* Feature turned off: wake immediately. */
        hw_lcd_activity();
    }
}

uint32_t hw_lcd_get_standby_timeout(void)
{
    return s_standby_enabled ? s_standby_timeout_ms : 0;
}

/* Record user activity: keeps the screen awake and wakes it if blanked.
 * Call this from key/press handlers or any "user is here" event. */
void hw_lcd_activity(void)
{
    s_last_activity_ms = esp_timer_get_time() / 1000;
    if (s_standby_active) {
        s_standby_active = false;
        if (s_lcd_display_on && s_lcd_io_handle) {
            st7789_tx_param(s_lcd_io_handle, ST7789_DISPON, NULL, 0);
            st7789_delay_ms(20);
        }
        /* Restore the user-configured brightness (PWM duty). */
        const uint32_t duty = (BL_DUTY_MAX * s_bl_percent) / 100;
        ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL, duty));
        ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL));
        ESP_LOGI(TAG, "Screen wake (backlight %u%%)", s_bl_percent);
    }
}

bool hw_lcd_is_standby_active(void)
{
    return s_standby_active;
}

/* Drive the idle timer. Call periodically from the main loop; it blanks the
 * screen once the timeout elapses and standby is enabled. */
void hw_lcd_standby_tick(void)
{
    if (!s_standby_enabled || s_standby_active) {
        return;
    }
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - s_last_activity_ms < (int64_t)s_standby_timeout_ms) {
        return;
    }

    s_standby_active = true;
    /* Turn the backlight fully off. */
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL, 0));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL));
    if (s_lcd_display_on && s_lcd_io_handle) {
        st7789_tx_param(s_lcd_io_handle, ST7789_DISPOFF, NULL, 0);
    }
    ESP_LOGI(TAG, "Screen standby after %u ms idle", s_standby_timeout_ms);
}
