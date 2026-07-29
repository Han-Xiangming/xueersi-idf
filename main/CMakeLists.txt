idf_component_register(SRCS "main.c"
                             "hardware/lcd.c"
                             "hardware/buzzer.c"
                             "hardware/sd.c"
                             "hardware/buttons.c"
                             "app/ui.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_lcd
                                     esp_timer
                                     esp_driver_gpio
                                     esp_driver_spi
                                     esp_driver_ledc
                                     esp_driver_sdspi
                                     esp_hw_support
                                     heap
                                     spi_flash
                                     fatfs
                                     sdmmc
                                     lvgl
                       )

target_link_libraries(${COMPONENT_LIB} PRIVATE m)
