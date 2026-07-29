idf_component_register(SRCS "main.c"
                             "hw/hw_lcd.c"
                             "hw/hw_buzzer.c"
                             "hw/hw_sd.c"
                             "hw/hw_buttons.c"
                             "sw/ui.c"
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
