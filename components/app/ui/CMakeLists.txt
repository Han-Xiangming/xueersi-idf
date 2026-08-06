idf_component_register(
    SRCS "ui.c"
    INCLUDE_DIRS "."
    REQUIRES audio
             board
             bt_audio
             buttons
             ebook
             esp_timer
             fonts
             lcd
             lvgl
             nvs_flash
             player
             sd
)
