idf_component_register(
    SRCS "player.c"
    INCLUDE_DIRS "."
    REQUIRES audio
             esp-libhelix-mp3
)
