idf_component_register(
    SRCS "ui.c"
         "ui_launcher_icons.c"
         "ui_pages_bt.c"
         "ui_pages_launcher.c"
         "ui_pages_ebook.c"
         "ui_pages_player.c"
         "ui_pages_settings.c"
         "ui_nav.c"
         "ui_theme.c"
         "ui_widgets.c"
    INCLUDE_DIRS "."
    REQUIRES audio
             battery
             board
             bluetooth_audio
             buttons
             ebook
             esp_timer
             fonts
             lcd
             lvgl
             nvs_flash
             player
             sd
             log_sink
)
