idf_component_register(
    SRCS "main.c"
    PRIV_REQUIRES audio
                  battery
                  board
                  bt_audio
                  buttons
                  ebook
                  fonts
                  lcd
                  lvgl
                  nvs_flash
                  player
                  sd
                  ui
)
