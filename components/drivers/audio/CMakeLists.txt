idf_component_register(
    SRCS "audio.c"
    INCLUDE_DIRS "."
    REQUIRES board
             bt_audio
             esp_driver_i2s
             speexdsp
)

# Match the SpeexDSP component's build flags so the resampler's renamed symbols
# (OUTSIDE_SPEEX + RANDOM_PREFIX) line up between audio.c and resample.c.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    FIXED_POINT
    OUTSIDE_SPEEX
    RANDOM_PREFIX=xueersi
)
target_link_libraries(${COMPONENT_LIB} PRIVATE m)
