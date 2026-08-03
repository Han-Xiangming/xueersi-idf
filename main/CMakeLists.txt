idf_component_register(
    SRCS "main.c"
    PRIV_REQUIRES audio
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
