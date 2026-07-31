idf_component_register(SRCS "main.c"
                             "hardware/lcd.c"
                             "hardware/audio.c"
                             "hardware/bt_audio.c"
                             "hardware/sd.c"
                             "hardware/buttons.c"
                             "app/ui.c"
                             "app/player.c"
                             "app/ebook.c"
                             "fonts/lv_font_cn_12.c"
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
                                     nvs_flash
                                     lvgl
                                     bt
                                     esp-libhelix-mp3
                       EMBED_FILES "tracks/Test.mp3"
                                   "text/Test.txt"
                       )

target_link_libraries(${COMPONENT_LIB} PRIVATE m)

# lv_font_cn_12.c is generated with LVGL's standard preamble; force the
# simple include so it resolves "lvgl.h" like the rest of the project.
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE=1)
