idf_component_register(
    SRCS "lv_font_cn_12.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)

# lv_font_cn_12.c is generated with LVGL's standard preamble; force the
# simple include so it resolves "lvgl.h" like the rest of the project.
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE=1)
