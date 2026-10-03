/*******************************************************************************
 * Size: 16 px
 * Bpp: 1
 * Opts: --font components/fonts/NotoSansSymbols2-Regular.ttf -r 0x23ED-0x23EF -r 0x23F8-0x23F9 --format lvgl --size 16 --bpp 1 --no-compress --no-kerning --force-fast-kern-format --lv-font-name lv_font_pause -o components/fonts/lv_font_pause.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

#ifndef LV_FONT_PAUSE
#define LV_FONT_PAUSE 1
#endif

#if LV_FONT_PAUSE

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+23ED "⏭" */
    0x84, 0x1e, 0x10, 0xf8, 0xe7, 0xe7, 0xbf, 0x3d,
    0xf1, 0xcf, 0x8, 0x70, 0x83,

    /* U+23EE "⏮" */
    0xc2, 0xe, 0x10, 0xf3, 0x8f, 0xbc, 0xfd, 0xe7,
    0xe7, 0x1f, 0x8, 0x78, 0x41,

    /* U+23EF "⏯" */
    0x86, 0x70, 0xcf, 0x99, 0xfb, 0x3f, 0x67, 0xcc,
    0xe1, 0x9c, 0x33,

    /* U+23F8 "⏸" */
    0xcf, 0x3c, 0xf3, 0xcf, 0x3c, 0xf3, 0xcf, 0x30,

    /* U+23F9 "⏹" */
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 233, .box_w = 13, .box_h = 8, .ofs_x = 1, .ofs_y = 1},
    {.bitmap_index = 13, .adv_w = 233, .box_w = 13, .box_h = 8, .ofs_x = 1, .ofs_y = 1},
    {.bitmap_index = 26, .adv_w = 233, .box_w = 11, .box_h = 8, .ofs_x = 2, .ofs_y = 1},
    {.bitmap_index = 37, .adv_w = 233, .box_w = 6, .box_h = 10, .ofs_x = 4, .ofs_y = 0},
    {.bitmap_index = 45, .adv_w = 233, .box_w = 9, .box_h = 8, .ofs_x = 3, .ofs_y = 1}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/

static const uint16_t unicode_list_0[] = {
    0x0, 0x1, 0x2, 0xb, 0xc
};

/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 9197, .range_length = 13, .glyph_id_start = 1,
        .unicode_list = unicode_list_0, .glyph_id_ofs_list = NULL, .list_length = 5, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 1,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t lv_font_pause = {
#else
lv_font_t lv_font_pause = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 10,          /*The maximum line height required by the font*/
    .base_line = 0,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -2,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if LV_FONT_PAUSE*/

