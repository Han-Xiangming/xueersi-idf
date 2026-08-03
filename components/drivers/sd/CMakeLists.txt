idf_component_register(
    SRCS "sd.c"
    INCLUDE_DIRS "."
    REQUIRES board
             esp_driver_sdspi
             esp_driver_spi
             fatfs
             sdmmc
)
