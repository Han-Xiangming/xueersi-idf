idf_component_register(
    SRCS "bt_audio.c"
    INCLUDE_DIRS "."
    REQUIRES bt
             esp_ringbuf
             esp_timer
)
