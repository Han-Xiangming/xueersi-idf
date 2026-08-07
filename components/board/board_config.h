/*
 * Board hardware configuration for Xiaomiao ESP32-WROVER-B.
 *
 * Pure hardware-layer description: SPI buses, LCD/SD pins, display
 * geometry and shared bus resources. No LVGL or application logic here.
 */
#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"

/* SPI bus shared by LCD and SD card */
#define LCD_HOST                SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ      (60 * 1000 * 1000)

/* Native (controller) and rotated (90 deg) display resolution */
#define LCD_NATIVE_H_RES        240
#define LCD_NATIVE_V_RES        320
#define LCD_H_RES               320
#define LCD_V_RES               240

/* Partial-refresh draw buffers: 40 of 240 lines
 * so LVGL renders and flushes only dirty regions instead of the whole panel.
 * Two buffers let LVGL render the next chunk while the previous one is being
 * flushed over SPI. */
#define LCD_DRAW_BUF_LINES      40
#define LCD_DRAW_BUF_COUNT      2
#define LCD_DPI                 60
#define LCD_CMD_BITS            8
#define LCD_PARAM_BITS          8
#define LCD_X_GAP               0
#define LCD_Y_GAP               0

/* LCD / ST7789 pins */
#define PIN_NUM_LCD_SCLK        GPIO_NUM_18
#define PIN_NUM_LCD_MOSI        GPIO_NUM_23
#define PIN_NUM_LCD_MISO        GPIO_NUM_19
#define PIN_NUM_LCD_CS          GPIO_NUM_5
#define PIN_NUM_LCD_DC          GPIO_NUM_4

/* LCD backlight (active-high PWM on GPIO 14, see lcd.c). */
#define PIN_NUM_LCD_BL          GPIO_NUM_14

/* SD card (SDSPI on shared SPI2) */
#define PIN_NUM_SD_CS           GPIO_NUM_22
#define SD_SPI_MAX_FREQ_KHZ     10000

/* I2S audio output (MAX98357 Class-D DAC, internal PLL, no MCLK) */
#define PIN_NUM_I2S_BCLK        GPIO_NUM_32
#define PIN_NUM_I2S_LRC         GPIO_NUM_15
#define PIN_NUM_I2S_DIN         GPIO_NUM_21

/* Battery level sensing (ADC1 channel 3 = GPIO 39).
 *
 * A 2-resistor divider scales the single-cell Li-ion pack down into the
 * ESP32 ADC range:
 *
 *     V_bat --[R_top]-- GPIO39 --[R_bot]-- GND
 *
 * A 2-resistor divider (two 0.1 MΩ / "01D" parts, R_top = R_bot = 100 kΩ)
 * scales the single-cell Li-ion pack down into the ESP32 ADC range:
 *
 *     V_bat --[R_top]-- GPIO39 --[R_bot]-- GND
 *
 * At full charge V_bat ≈ 4.20 V → GPIO ≈ 2.10 V (well inside the 0..3.3 V
 * ADC span). The divider ratio is 1/2, so V_bat = V_adc * (R_top + R_bot)/R_bot. */
#define PIN_NUM_BAT_ADC         GPIO_NUM_39
#define BAT_DIV_R_TOP_KOHM      100     /* upper divider resistor (kΩ) */
#define BAT_DIV_R_BOT_KOHM      100     /* lower divider resistor (kΩ) */
/* V_bat = V_adc * BAT_DIV_FACTOR. (100+100)/100 = 2.0 */
#define BAT_DIV_FACTOR          ((float)(BAT_DIV_R_TOP_KOHM + BAT_DIV_R_BOT_KOHM) / BAT_DIV_R_BOT_KOHM)

/* Li-ion cell voltage points used for the percent mapping (single cell). */
#define BAT_V_FULL              4.20f   /* 100% */
#define BAT_V_EMPTY            3.30f   /* 0%   (cut-off) */
