/*
 * Hardware layer: ST7789 LCD driver and LVGL display binding.
 * See lcd.h.
 */
#include <assert.h>
#include <math.h>
#include <string.h>
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

/* Non-linear (gamma-corrected) brightness curve. Human perception of
 * brightness is roughly logarithmic, so a linear 0..100% -> 0..1023 duty
 * map makes low steps invisible and high steps jumpy. The (BL_LUT_STEPS+1)
 * entry LUT (index 0..BL_LUT_STEPS) is built at run time so the dimmest
 * visible level is a tunable floor rather than collapsing to 0:
 *
 *     lut[0]                       = 0                (the ONLY off level)
 *     lut[i], i=1..BL_LUT_STEPS    = round(BL_LUT_MIN_ON
 *                                 + (BL_DUTY_MAX - BL_LUT_MIN_ON)
 *                                   * ((i-1)/(BL_LUT_STEPS-1))^BL_LUT_GAMMA)
 *
 * This guarantees a strictly increasing, always-visible range from
 * BL_LUT_MIN_ON (dimmest) up to full brightness, with exactly one black
 * level at index 0. Without the MIN_ON floor the first two entries both
 * round to 0, creating a dark "dead zone" where two presses are needed to
 * escape black. Tune BL_LUT_MIN_ON against real hardware. BL_LUT_STEPS
 * mirrors the public HW_LCD_BACKLIGHT_STEPS in lcd.h. */
#define BL_LUT_STEPS          HW_LCD_BACKLIGHT_STEPS
#define BL_LUT_MIN_ON         30      /* dimmest visible 10-bit duty (0..1023) */
#define BL_LUT_GAMMA          2.2f
static uint16_t s_bl_lut[BL_LUT_STEPS + 1];

/* Fill s_bl_lut[] with the gamma curve described above. Called once from
 * backlight_init() before any duty is applied. */
static void backlight_lut_init(void)
{
    s_bl_lut[0] = 0;   /* off */
    for (int i = 1; i <= BL_LUT_STEPS; i++) {
        float t = (float)(i - 1) / (BL_LUT_STEPS - 1);
        float duty = BL_LUT_MIN_ON + (BL_DUTY_MAX - BL_LUT_MIN_ON) * powf(t, BL_LUT_GAMMA);
        int d = (int)(duty + 0.5f);
        if (d > BL_DUTY_MAX) {
            d = BL_DUTY_MAX;
        }
        s_bl_lut[i] = (uint16_t)d;
    }
}

/* Map a 0..100 % brightness to the nearest LUT level (0..BL_LUT_STEPS). */
static inline uint8_t bl_percent_to_step(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    return (uint8_t)(((uint16_t)percent * BL_LUT_STEPS + 50) / 100);
}

/* Map a 0..100 % brightness to a 10-bit LEDC duty via the gamma LUT. */
static inline uint32_t bl_percent_to_duty(uint8_t percent)
{
    return s_bl_lut[bl_percent_to_step(percent)];
}

static bool s_bl_inited;
static uint8_t s_bl_step = BL_LUT_STEPS;   /* current level 0..BL_LUT_STEPS, full by default */

/* Flush completion tracking. lvgl_flush_cb() queues one tx_color per window
 * (LCD_FLUSH_BATCH_ROWS rows, see below); every queued transfer produces one
 * lcd_flush_ready_cb() from the SPI ISR. s_flush_pending counts how many window
 * transfers are still in flight, and lv_display_flush_ready() is signalled
 * exactly once per flush — when the LAST window's DMA completes (success path)
 * or after the drain in lcd_flush_abort() (error path, where no DMA
 * completes). Signalling early
 * (e.g. after the first row, as a plain "pending" bool would) lets LVGL
 * reuse the draw buffer while the remaining rows are still being DMA-read
 * out of px_map, which tears the panel.
 *
 * Both counters are touched from the SPI ISR and from the LVGL render task,
 * so every access goes through the s_flush_mux critical section. The counter
 * is ADDITIVE at flush start (s_flush_pending += windows, not an overwrite): a
 * late completion callback from a previous flush can overlap the start of the
 * next flush, and an overwrite would drop that window so the next flush would
 * release LVGL one window too early. */
static portMUX_TYPE s_flush_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int32_t s_flush_pending = 0;    /* window DMA transfers in flight */
static volatile bool s_flush_ready_sent = false; /* lv_display_flush_ready() delivered */

/* DMA bounce buffers for window colour transfers. esp_lcd_panel_io_tx_color()
 * allocates a private TX buffer per call (a copy into non-cached internal DMA
 * memory) whenever the source is not DMA-safe — e.g. the draw buffers, which
 * live in cached PSRAM on ESP32-S3. Under memory pressure that allocation
 * fails (ESP_ERR_NO_MEM -> "flush line ... failed"). We instead keep a small
 * ring of DMA-capable, non-cached bounce buffers and copy each window into one
 * before queueing, so the SPI layer never needs to allocate.
 *
 * Rows per window: one CASET+RASET+RAMWR transaction set per
 * LCD_FLUSH_BATCH_ROWS rows instead of per row. A full-screen flush drops from
 * 240x3 transactions to 20x3. Two wins:
 *   1. per-transaction bus-lock / CS / command overhead is paid once per batch;
 *   2. each SPI completion ISR preempts core 0 (where the decode task and the
 *      Bluetooth stack live) for its duration, so per-row ISRs were a hidden
 *      tax on playback — quartering them matters more than the bus time.
 *
 * The ring must stay deeper than the LCD io queue (trans_queue_depth, now 1:
 * one transaction queued + one in flight) so a slot is never reused while its
 * transfer is still queued. Slots are LCD_FLUSH_BATCH_ROWS rows deep each. */
#define LCD_FLUSH_BATCH_ROWS     4
#define LCD_FLUSH_BOUNCE_COUNT   3
static uint8_t *s_flush_bounce[LCD_FLUSH_BOUNCE_COUNT];
/* Rotating bounce-slot index, advanced once per queued window. Module-level so
 * st7789_clear_black() and lvgl_flush_cb() share one ring. */
static int s_flush_slot;

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

/* Queue one rectangular window as a single CASET+RASET+RAMWR transaction set,
 * copying `bytes` from `src` through the next DMA bounce slot. Returns false
 * (error logged) if any esp_lcd call failed; the caller then aborts the flush
 * with the windows it had NOT yet queued. */
static bool lcd_send_window(esp_lcd_panel_io_handle_t io_handle,
                            uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2,
                            const uint8_t *src, size_t bytes)
{
    const uint8_t caset[] = {
        (uint8_t)(((uint16_t)(x1 + LCD_X_GAP)) >> 8),
        (uint8_t)(((uint16_t)(x1 + LCD_X_GAP)) & 0xFF),
        (uint8_t)(((uint16_t)(x2 + LCD_X_GAP)) >> 8),
        (uint8_t)(((uint16_t)(x2 + LCD_X_GAP)) & 0xFF),
    };
    const uint8_t raset[] = {
        (uint8_t)(((uint16_t)(y1 + LCD_Y_GAP)) >> 8),
        (uint8_t)(((uint16_t)(y1 + LCD_Y_GAP)) & 0xFF),
        (uint8_t)(((uint16_t)(y2 + LCD_Y_GAP)) >> 8),
        (uint8_t)(((uint16_t)(y2 + LCD_Y_GAP)) & 0xFF),
    };

    esp_err_t err = esp_lcd_panel_io_tx_param(io_handle,
                                              ST7789_CASET, caset, sizeof(caset));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "window caset failed: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_lcd_panel_io_tx_param(io_handle,
                                    ST7789_RASET, raset, sizeof(raset));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "window raset failed: %s", esp_err_to_name(err));
        return false;
    }

    uint8_t *bb = s_flush_bounce[s_flush_slot % LCD_FLUSH_BOUNCE_COUNT];
    s_flush_slot++;
    memcpy(bb, src, bytes);
    err = esp_lcd_panel_io_tx_color(io_handle, ST7789_RAMWR, bb, bytes);
    if (err != ESP_OK) {
        /* Never abort on a transient transfer error: just unlock the display
         * so LVGL can retry next frame instead of deadlocking or crashing the
         * whole firmware. */
        ESP_LOGW(TAG, "window y=%u..%u failed: %s",
                 (unsigned)y1, (unsigned)y2, esp_err_to_name(err));
        return false;
    }
    return true;
}

/* Fill the whole panel black (boot / standby). Sent in batched windows, same
 * as lvgl_flush_cb(). */
static void st7789_clear_black(esp_lcd_panel_io_handle_t io_handle)
{
    /* Dedicated all-zero source: lcd_send_window() copies it into a bounce
     * slot per window. Kept OUTSIDE the ring so the copy never aliases a slot
     * that is still queued or in flight. */
    static uint8_t black_src[LCD_H_RES * sizeof(uint16_t) * LCD_FLUSH_BATCH_ROWS];
    memset(black_src, 0, sizeof(black_src));

    for (uint16_t y = 0; y < LCD_V_RES; y += LCD_FLUSH_BATCH_ROWS) {
        const uint16_t rows = (LCD_V_RES - y < LCD_FLUSH_BATCH_ROWS)
                            ? (LCD_V_RES - y) : LCD_FLUSH_BATCH_ROWS;
        ESP_ERROR_CHECK(lcd_send_window(io_handle, 0, y, LCD_H_RES - 1, y + rows - 1,
                                        black_src,
                                        LCD_H_RES * sizeof(uint16_t) * rows)
                        ? ESP_OK : ESP_FAIL);
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

    /* Runs in the SPI ISR: one call per queued tx_color DMA transfer. The
     * last in-flight transfer of a flush brings the counter to 0, and only
     * then is LVGL released. The s_flush_ready_sent guard keeps the release
     * to exactly once even if an abort drain races this callback. */
    lv_display_t *display = (lv_display_t *)user_ctx;
    bool signal_ready = false;
    portENTER_CRITICAL_ISR(&s_flush_mux);
    if (--s_flush_pending <= 0 && !s_flush_ready_sent) {
        s_flush_ready_sent = true;
        s_lcd_first_flush_done = true;
        signal_ready = true;
    }
    portEXIT_CRITICAL_ISR(&s_flush_mux);
    if (signal_ready) {
        lv_display_flush_ready(display);
    }
    return false;
}

/* Error path for lvgl_flush_cb: windows that were never queued will never get
 * a completion callback, so drop them from the counter first, then drain-wait
 * for the in-flight transfers before releasing LVGL exactly once. Skipping
 * the drain would let a late callback corrupt the next flush's count and
 * release LVGL too early. */
static void lcd_flush_abort(lv_display_t *display, int windows_total, int windows_queued)
{
    portENTER_CRITICAL(&s_flush_mux);
    s_flush_pending -= (windows_total - windows_queued);
    portEXIT_CRITICAL(&s_flush_mux);

    int wait = 0;
    while (s_flush_pending > 0 && wait++ < 200) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (wait >= 200) {
        ESP_LOGW(TAG, "flush abort: %d window transfers still pending", (int)s_flush_pending);
    }

    portENTER_CRITICAL(&s_flush_mux);
    const bool needs_signal = !s_flush_ready_sent;
    if (needs_signal) {
        s_flush_ready_sent = true;
        s_lcd_first_flush_done = true;
    }
    portEXIT_CRITICAL(&s_flush_mux);
    if (needs_signal) {
        lv_display_flush_ready(display);
    }
}

static void lvgl_flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
    const int width = area->x2 - area->x1 + 1;
    const int rows = area->y2 - area->y1 + 1;
    const size_t line_bytes = (size_t)width * sizeof(uint16_t);
    const uint8_t *line = px_map;

    /* Arm the completion counter for this flush: every window queued below
     * will later produce one lcd_flush_ready_cb() from the SPI ISR, and LVGL
     * is released only when the last of them completes. Additive, so a late
     * callback from the previous flush (one that started before this arm) is
     * still counted against that flush, not this one. */
    const int windows = (rows + LCD_FLUSH_BATCH_ROWS - 1) / LCD_FLUSH_BATCH_ROWS;
    portENTER_CRITICAL(&s_flush_mux);
    s_flush_pending += windows;
    s_flush_ready_sent = false;
    portEXIT_CRITICAL(&s_flush_mux);
    int windows_queued = 0;
    int rows_queued = 0;

    /* Send the dirty rectangle in batched windows of up to
     * LCD_FLUSH_BATCH_ROWS rows: one CASET+RASET+RAMWR set per window instead
     * of three transactions per row (see LCD_FLUSH_BATCH_ROWS for why). The
     * window's rows are contiguous in px_map — LVGL reshapes the partial-render
     * draw buffer to the flush area (lv_refr.c: layer_reshape_draw_buf), so the
     * row pitch is the area width — which makes the copy a single memcpy. */
    while (rows_queued < rows) {
        const int batch = (rows - rows_queued < LCD_FLUSH_BATCH_ROWS)
                        ? (rows - rows_queued) : LCD_FLUSH_BATCH_ROWS;
        const uint16_t y1 = (uint16_t)(area->y1 + rows_queued);
        const uint16_t y2 = (uint16_t)(y1 + batch - 1);

        if (!lcd_send_window(s_lcd_io_handle, (uint16_t)area->x1, y1,
                             (uint16_t)area->x2, y2,
                             line, line_bytes * (size_t)batch)) {
            lcd_flush_abort(display, windows, windows_queued);
            return;
        }
        rows_queued += batch;
        windows_queued++;
        line += line_bytes * (size_t)batch;
    }

    /* Success path: every tx_color above is asynchronous (DMA). The last
     * one's completion brings s_flush_pending to 0 in lcd_flush_ready_cb,
     * which signals LVGL exactly once. Do NOT call lv_display_flush_ready()
     * here — that would release the draw buffers while rows are still in
     * flight. */
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

    /* Allocate the window-transfer bounce ring from the SPI bus's DMA-capable
     * (non-cached, internal) pool. Each slot holds one batched window
     * (LCD_FLUSH_BATCH_ROWS rows); allocated once, so it never contends with
     * the per-frame priv-buffer churn that was failing. */
    for (int i = 0; i < LCD_FLUSH_BOUNCE_COUNT; i++) {
        s_flush_bounce[i] = spi_bus_dma_memory_alloc(
            LCD_HOST, LCD_H_RES * sizeof(uint16_t) * LCD_FLUSH_BATCH_ROWS, 0);
        assert(s_flush_bounce[i]);
    }

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    /* 队列深度 1（原 10 → 3 → 1）：SPI2 与 SDSPI 共用（见 board_config.h），
     * spi_bus_lock 按设备 ID 仲裁，LCD（先注册，dev 0）在双方都有待发事务时
     * 永远赢——SD 事务只能等 LCD 队列排空后才能插队。深队列时连续刷屏的
     * LVGL 任务持续补货、队列从不排空，一次 4 KB SD 读（~11 笔事务）被整体
     * 推迟到 flush 流结束；解码停摆超过 ring+DMA 裕量即欠载静音，听感为
     * 播放"变慢"。
     * 配合窗口批处理（LCD_FLUSH_BATCH_ROWS）：单笔事务变大（4 行 ≈ 340 us
     * @60 MHz）但深度 1 保证 SD 最多等 1 笔，等待窗口与旧的"深度 3 x 1 行"
     * (~0.3 ms) 持平；而每 flush 的事务数、总线锁/CS/命令开销与 SPI 完成
     * ISR（每次都会抢占 Core 0 的解码/BT 任务）都降到 1/4。副作用是好的：
     * LVGL 更频繁阻塞在队列满处，反而把 Core 1 让给 i2s_wr(prio 6)。
     * bounce ring 为 3 槽 >= 深度 1（1 笔在队 + 1 笔在飞），槽位安全性
     * 由 s_flush_slot 轮转保证。 */
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_NUM_LCD_DC,
        .cs_gpio_num = PIN_NUM_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 1,
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

    backlight_lut_init();   /* build gamma LUT before any duty is set */

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

/* Set backlight brightness as a percentage (0..100). Clamped to range and
 * snapped to the nearest gamma level. Kept for config restore / legacy callers. */
void hw_lcd_set_backlight(uint8_t percent)
{
    if (!s_bl_inited) {
        backlight_init();
    }
    s_bl_step = bl_percent_to_step(percent);
    const uint32_t duty = s_bl_lut[s_bl_step];
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL));
}

/* Step the backlight by delta levels (negative = dimmer) and apply it
 * immediately. Clamped to 0..BL_LUT_STEPS. Returns the resulting brightness
 * as a percentage (0..100), which the UI can persist straight to NVS. */
uint8_t hw_lcd_step_backlight(int8_t delta)
{
    if (!s_bl_inited) {
        backlight_init();
    }
    int step = (int)s_bl_step + (int)delta;
    if (step < 0) {
        step = 0;
    } else if (step > BL_LUT_STEPS) {
        step = BL_LUT_STEPS;
    }
    s_bl_step = (uint8_t)step;
    const uint32_t duty = s_bl_lut[s_bl_step];
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL));
    return (uint8_t)(((uint16_t)s_bl_step * 100 + (BL_LUT_STEPS / 2)) / BL_LUT_STEPS);
}

/* Set the backlight directly by level (0..BL_LUT_STEPS). */
void hw_lcd_set_backlight_step(uint8_t step)
{
    if (step > BL_LUT_STEPS) {
        step = BL_LUT_STEPS;
    }
    if (!s_bl_inited) {
        backlight_init();
    }
    s_bl_step = step;
    const uint32_t duty = s_bl_lut[s_bl_step];
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL));
}

/* Current backlight level (0..BL_LUT_STEPS). */
uint8_t hw_lcd_get_backlight_step(void)
{
    return s_bl_inited ? s_bl_step : 0;
}

uint8_t hw_lcd_get_backlight(void)
{
    if (!s_bl_inited) {
        return 0;
    }
    return (uint8_t)(((uint16_t)s_bl_step * 100 + (BL_LUT_STEPS / 2)) / BL_LUT_STEPS);
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
        const uint32_t duty = s_bl_lut[s_bl_step];
        ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL, duty));
        ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL));
        ESP_LOGI(TAG, "Screen wake (backlight %u/%u)", s_bl_step, BL_LUT_STEPS);
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
