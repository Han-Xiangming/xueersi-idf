idf_component_register(
    SRCS "src/resample.c"
    INCLUDE_DIRS "include" "include/speex" "src"
)

# Build SpeexDSP resampler as a self-contained, fixed-point library.
# OUTSIDE_SPEEX + RANDOM_PREFIX avoid symbol clashes if Speex is linked later.
target_compile_definitions(${COMPONENT_TARGET} PRIVATE
    FIXED_POINT
    OUTSIDE_SPEEX
    RANDOM_PREFIX=xueersi
)
target_compile_options(${COMPONENT_TARGET} PRIVATE -Wno-unused-function)
