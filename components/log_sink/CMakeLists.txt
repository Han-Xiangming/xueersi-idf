idf_component_register(
    SRCS "log_sink.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES sd
                 freertos
)
