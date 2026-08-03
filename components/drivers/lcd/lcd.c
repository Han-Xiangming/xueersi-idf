/*
 * Hardware layer: ST7735 LCD driver and LVGL display binding.
 * See hardware/lcd.h.
 */
#include <assert.h>
#include <sys/param.h>

#include "board_config.h"
#include "lcd.h"

#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "hw_lcd";

/* ST7735 command set */
#define ST7735_SWRESET              0x01
#define ST7735_SLPOUT               0x11
#define ST7735_NORON                0x13
#define ST7735_INVOFF               0x20
#define ST7735_DISPOFF              0x28
#define ST7735_DISPON               0x29
#define ST7735_CASET                0x2A
#define ST7735_RASET                0x2B
#define ST7735_RAMWR                0x2C
#define ST7735_MADCTL               0x36
#define ST7735_COLMOD               0x3A
#define ST7735_FRMCTR1              0xB1
#define ST7735_FRMCTR2              0xB2
#define ST7735_FRMCTR3              0xB3
#define ST7735_INVCTR               0xB4
#define ST7735_PWCTR1               0xC0
#define ST7735_PWCTR2               0xC1
#define ST7735_PWCTR3               0xC2
#define ST7735_PWCTR4               0xC3
#define ST7735_PWCTR5               0xC4
#define ST7735_VMCTR1               0xC5
#define ST7735_GMCTRP1              0xE0
#define ST7735_GMCTRN1              0xE1

#define MADCTL_MY                   0x80
#define MADCTL_MX                   0x40
#define MADCTL_MV                   0x20
#define MADCTL_RGB                  0x00

static esp_lcd_panel_io_handle_t s_lcd_io_handle;
static bool s_lcd_display_on;
static volatile bool s_lcd_first_flush_done;
static lv_draw_buf_t s_draw_buf3;

static void st7735_tx_param(esp_lcd_panel_io_handle_t io_handle, int cmd, const void *param, size_t param_size)
{
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, cmd, param, param_size));
}

static void st7735_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void st7735_clear_black(esp_lcd_panel_io_handle_t io_handle)
{
    static uint16_t line[LCD_H_RES * 8];
    const uint8_t caset[] = {0x00, 0x00, 0x00, LCD_H_RES - 1};

    memset(line, 0, sizeof(line));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, ST7735_CASET, caset, sizeof(caset)));
    for (uint16_t y = 0; y < LCD_V_RES; y += 8) {
        const uint16_t y2 = MIN((uint16_t)(y + 7), (uint16_t)(LCD_V_RES - 1));
        const uint8_t raset[] = {
            y >> 8, y & 0xFF,
            y2 >> 8, y2 & 0xFF,
        };
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, ST7735_RASET, raset, sizeof(raset)));
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(io_handle, ST7735_RAMWR, line, (y2 - y + 1) * LCD_H_RES * sizeof(uint16_t)));
    }
}

static void st7735_init_black_tab_rot90(esp_lcd_panel_io_handle_t io_handle)
{
    const uint8_t frmctr[] = {0x01, 0x2C, 0x2D};
    const uint8_t frmctr3[] = {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D};
    const uint8_t invctr[] = {0x07};
    const uint8_t pwctr1[] = {0xA2, 0x02, 0x84};
    const uint8_t pwctr2[] = {0xC5};
    const uint8_t pwctr3[] = {0x0A, 0x00};
    const uint8_t pwctr4[] = {0x8A, 0x2A};
    const uint8_t pwctr5[] = {0x8A, 0xEE};
    const uint8_t vmctr1[] = {0x0E};
    const uint8_t madctl_default[] = {MADCTL_MX | MADCTL_MY | MADCTL_RGB};
    const uint8_t colmod[] = {0x05};
    const uint8_t caset[] = {0x00, 0x00, 0x00, LCD_NATIVE_H_RES - 1};
    const uint8_t raset[] = {0x00, 0x00, 0x00, LCD_NATIVE_V_RES - 1};
    const uint8_t gamma_pos[] = {
        0x02, 0x1C, 0x07, 0x12,
        0x37, 0x32, 0x29, 0x2D,
        0x29, 0x25, 0x2B, 0x39,
        0x00, 0x01, 0x03, 0x10,
    };
    const uint8_t gamma_neg[] = {
        0x03, 0x1D, 0x07, 0x06,
        0x2E, 0x2C, 0x29, 0x2D,
        0x2E, 0x2E, 0x37, 0x3F,
        0x00, 0x00, 0x02, 0x10,
    };
    const uint8_t madctl_rot90[] = {MADCTL_MX | MADCTL_MV | MADCTL_RGB};

    ESP_LOGI(TAG, "Initialize ST7735R panel with MicroPython init(2) compatible sequence");
    st7735_tx_param(io_handle, ST7735_DISPOFF, NULL, 0);
    st7735_tx_param(io_handle, ST7735_SWRESET, NULL, 0);
    st7735_delay_ms(150);
    st7735_tx_param(io_handle, ST7735_SLPOUT, NULL, 0);
    st7735_delay_ms(500);
    st7735_tx_param(io_handle, ST7735_FRMCTR1, frmctr, sizeof(frmctr));
    st7735_tx_param(io_handle, ST7735_FRMCTR2, frmctr, sizeof(frmctr));
    st7735_tx_param(io_handle, ST7735_FRMCTR3, frmctr3, sizeof(frmctr3));
    st7735_tx_param(io_handle, ST7735_INVCTR, invctr, sizeof(invctr));
    st7735_tx_param(io_handle, ST7735_PWCTR1, pwctr1, sizeof(pwctr1));
    st7735_tx_param(io_handle, ST7735_PWCTR2, pwctr2, sizeof(pwctr2));
    st7735_tx_param(io_handle, ST7735_PWCTR3, pwctr3, sizeof(pwctr3));
    st7735_tx_param(io_handle, ST7735_PWCTR4, pwctr4, sizeof(pwctr4));
    st7735_tx_param(io_handle, ST7735_PWCTR5, pwctr5, sizeof(pwctr5));
    st7735_tx_param(io_handle, ST7735_VMCTR1, vmctr1, sizeof(vmctr1));
    st7735_tx_param(io_handle, ST7735_INVOFF, NULL, 0);
    st7735_tx_param(io_handle, ST7735_MADCTL, madctl_default, sizeof(madctl_default));
    st7735_tx_param(io_handle, ST7735_COLMOD, colmod, sizeof(colmod));
    st7735_tx_param(io_handle, ST7735_CASET, caset, sizeof(caset));
    st7735_tx_param(io_handle, ST7735_RASET, raset, sizeof(raset));
    st7735_tx_param(io_handle, ST7735_GMCTRP1, gamma_pos, sizeof(gamma_pos));
    st7735_tx_param(io_handle, ST7735_GMCTRN1, gamma_neg, sizeof(gamma_neg));
    st7735_tx_param(io_handle, ST7735_NORON, NULL, 0);
    st7735_delay_ms(10);
    st7735_tx_param(io_handle, ST7735_MADCTL, madctl_rot90, sizeof(madctl_rot90));
    st7735_clear_black(io_handle);
}

static void lcd_display_on(void)
{
    if (s_lcd_display_on || !s_lcd_io_handle) {
        return;
    }

    st7735_tx_param(s_lcd_io_handle, ST7735_DISPON, NULL, 0);
    st7735_delay_ms(20);
    s_lcd_display_on = true;
}

static bool lcd_flush_ready_cb(esp_lcd_panel_io_handle_t panel_io,
                               esp_lcd_panel_io_event_data_t *edata,
                               void *user_ctx)
{
    (void)panel_io;
    (void)edata;

    lv_display_t *display = (lv_display_t *)user_ctx;
    s_lcd_first_flush_done = true;
    lv_display_flush_ready(display);
    return false;
}

static void lvgl_flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_io_handle_t io_handle = lv_display_get_user_data(display);
    const int width = area->x2 - area->x1 + 1;
    const int height = area->y2 - area->y1 + 1;
    const uint16_t x_start = area->x1 + LCD_X_GAP;
    const uint16_t x_end = area->x2 + LCD_X_GAP;
    const uint16_t y_start = area->y1 + LCD_Y_GAP;
    const uint16_t y_end = area->y2 + LCD_Y_GAP;
    const uint8_t caset[] = {
        x_start >> 8, x_start & 0xFF,
        x_end >> 8, x_end & 0xFF,
    };
    const uint8_t raset[] = {
        y_start >> 8, y_start & 0xFF,
        y_end >> 8, y_end & 0xFF,
    };

    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, ST7735_CASET, caset, sizeof(caset)));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, ST7735_RASET, raset, sizeof(raset)));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(io_handle, ST7735_RAMWR, px_map, width * height * sizeof(uint16_t)));
}

void hw_lcd_init(void)
{
    ESP_LOGI(TAG, "Initialize SPI bus for ST7735 TFT");
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
    st7735_init_black_tab_rot90(io_handle);
}

/* Full-screen LVGL draw buffer. Prefer external PSRAM (DMA-capable); fall back
 * to the internal DMA pool if PSRAM is not present. */
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
#if LCD_DRAW_BUF_LINES != LCD_V_RES
#error "Triple/full refresh mode requires LCD_DRAW_BUF_LINES to equal LCD_V_RES"
#endif
#if LCD_DRAW_BUF_COUNT != 3
#error "This build is configured for full-screen triple buffering"
#endif

    lv_display_t *display = lv_display_create(LCD_H_RES, LCD_V_RES);
    assert(display);

    const lv_color_format_t color_format = LV_COLOR_FORMAT_RGB565_SWAPPED;
    const uint32_t stride = lv_draw_buf_width_to_stride(LCD_H_RES, color_format);
    const size_t draw_buffer_sz = stride * LCD_DRAW_BUF_LINES;
    /* Allocate the full-screen draw buffers from external PSRAM (DMA-capable)
     * first; fall back to the internal DMA pool if PSRAM is unavailable.
     * Using only spi_bus_dma_memory_alloc() draws from the very small internal
     * DMA pool and the 3rd triple-buffer allocation fails on boot. */
    void *buf1 = alloc_draw_buf(draw_buffer_sz);
    void *buf2 = alloc_draw_buf(draw_buffer_sz);
    void *buf3 = alloc_draw_buf(draw_buffer_sz);
    assert(buf1);
    assert(buf2);
    assert(buf3);

    lv_display_set_color_format(display, color_format);
    lv_display_set_dpi(display, LCD_DPI);
    lv_display_set_buffers(display, buf1, buf2, draw_buffer_sz, LV_DISPLAY_RENDER_MODE_FULL);
    lv_result_t res = lv_draw_buf_init(&s_draw_buf3,
                                       LCD_H_RES,
                                       LCD_DRAW_BUF_LINES,
                                       color_format,
                                       stride,
                                       buf3,
                                       draw_buffer_sz);
    assert(res == LV_RESULT_OK);
    lv_display_set_3rd_draw_buffer(display, &s_draw_buf3);
    lv_display_set_user_data(display, s_lcd_io_handle);
    lv_display_set_flush_cb(display, lvgl_flush_cb);

    esp_lcd_panel_io_callbacks_t callbacks = {
        .on_color_trans_done = lcd_flush_ready_cb,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(s_lcd_io_handle, &callbacks, display));

    ESP_LOGI(TAG,
             "LVGL display: %dx%d, dpi=%d, %d full-screen DMA buffers, SPI=%d MHz",
             LCD_H_RES,
             LCD_V_RES,
             LCD_DPI,
             LCD_DRAW_BUF_COUNT,
             LCD_PIXEL_CLOCK_HZ / 1000000);

    return display;
}

void hw_lcd_display_on(void)
{
    lcd_display_on();
}

bool hw_lcd_first_flush_done(void)
{
    return s_lcd_first_flush_done;
}
