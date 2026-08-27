idf_component_register(
    SRCS "battery.c"
    INCLUDE_DIRS "."
    REQUIRES board
             audio
             esp_adc
             esp_timer
             freertos
)
