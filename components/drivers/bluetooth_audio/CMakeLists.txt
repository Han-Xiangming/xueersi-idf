idf_component_register(
    SRCS "bluetooth_audio.c"
    INCLUDE_DIRS "."
    REQUIRES bt
             esp_ringbuf
             esp_timer
)
