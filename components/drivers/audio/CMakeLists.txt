idf_component_register(
    SRCS "audio.c"
    INCLUDE_DIRS "."
    REQUIRES board
             bt_audio
             esp_driver_i2s
             esp_ringbuf
)

target_link_libraries(${COMPONENT_LIB} PRIVATE m)
