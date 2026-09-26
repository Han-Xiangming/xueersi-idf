idf_component_register(
    SRCS "ebook.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash ebook_progress
)
