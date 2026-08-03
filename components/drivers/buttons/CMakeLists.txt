idf_component_register(
    SRCS "buttons.c"
    INCLUDE_DIRS "."
    REQUIRES board
             esp_driver_gpio
             lvgl
)
