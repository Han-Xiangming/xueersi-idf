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
#define LCD_NATIVE_H_RES        128
#define LCD_NATIVE_V_RES        160
#define LCD_H_RES               160
#define LCD_V_RES               128

#define LCD_DRAW_BUF_LINES      LCD_V_RES
#define LCD_DRAW_BUF_COUNT      3
#define LCD_DPI                 60
#define LCD_CMD_BITS            8
#define LCD_PARAM_BITS          8
#define LCD_X_GAP               0
#define LCD_Y_GAP               0

/* LCD / ST7735 pins */
#define PIN_NUM_LCD_SCLK        GPIO_NUM_18
#define PIN_NUM_LCD_MOSI        GPIO_NUM_23
#define PIN_NUM_LCD_MISO        GPIO_NUM_19
#define PIN_NUM_LCD_CS          GPIO_NUM_5
#define PIN_NUM_LCD_DC          GPIO_NUM_4

/* SD card (SDSPI on shared SPI2) */
#define PIN_NUM_SD_CS           GPIO_NUM_22
#define SD_SPI_MAX_FREQ_KHZ     10000

/* Passive buzzer (LEDC PWM) */
#define PIN_NUM_BUZZER          GPIO_NUM_14
