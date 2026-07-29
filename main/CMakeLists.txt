idf_component_register(SRCS "main.c"
                             "hardware/lcd.c"
                             "hardware/audio.c"
                             "hardware/bt_audio.c"
                             "hardware/sd.c"
                             "hardware/buttons.c"
                             "app/ui.c"
                             "app/player.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_lcd
                                     esp_timer
                                     esp_driver_gpio
                                     esp_driver_spi
                                     esp_driver_i2s
                                     esp_driver_sdspi
                                     esp_hw_support
                                     heap
                                     spi_flash
                                     fatfs
                                     sdmmc
                                     lvgl
                                     bt
                                     esp-libhelix-mp3
                       EMBED_FILES "tracks/1ezyZip.mp3"
                       )

target_link_libraries(${COMPONENT_LIB} PRIVATE m)
