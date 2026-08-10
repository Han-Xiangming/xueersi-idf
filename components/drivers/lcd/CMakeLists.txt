idf_component_register(
    SRCS "lcd.c"
    INCLUDE_DIRS "."
    REQUIRES board
             esp_driver_ledc
             esp_driver_spi
             esp_lcd
             esp_timer
             freertos
             lvgl
)
