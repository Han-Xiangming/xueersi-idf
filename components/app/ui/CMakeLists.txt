idf_component_register(
    SRCS "ui.c"
    INCLUDE_DIRS "."
    REQUIRES audio
             battery
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
