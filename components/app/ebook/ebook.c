/*
 * Application layer: TXT ebook reader.
 * See app/ebook.h and docs/ebook.md.
 */
#define LOG_LOCAL_LEVEL ESP_LOG_INFO
#include "ebook.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

static const char *TAG = "ebook";

/* --- Tuning constants (docs/ebook.md) --- */
#define EBOOK_NAME_MAX      64           /* book name buffer size */
#define EBOOK_LIST_MAX      64           /* max books in the scan list */
#define EBOOK_CHUNK         (4 * 1024)   /* stream window of the reader */
#define EBOOK_PAGE_BUF      1024         /* rendered page text buffer */
#define EBOOK_PAGE_LINES    8            /* lines per page. Body label starts at
                                          * y=36 and the status bar at y=214, leaving
                                          * ~178 px; at 22 px/row that holds 8 rows
                                          * (8*22=176) without colliding with the bar. */
#define EBOOK_LINE_W        320          /* text-area width, px (= LCD_H_RES, full screen)
                                          * Raised from 304 (LCD_H_RES-16) so a line can
                                          * hold 20 full-width glyphs (20*16=320) instead of
                                          * 19, killing the trailing right margin. The UI
                                          * label is sized to match (see ui.c). */
#define EBOOK_LINE_W16      (EBOOK_LINE_W * 16)  /* width in 1/16 px */
#define EBOOK_CHAR_W16      256          /* fullwidth advance, 1/16 px = 16 px */
#define EBOOK_HIST_N        32           /* page-start offset history ring */
#define EBOOK_ROM_PREFIX    "(ROM)"

/* Built-in test book compiled into the firmware via EMBED_FILES
 * (IDF names the symbols from the file basename only). */
extern const uint8_t _binary_Test_txt_start[];
extern const uint8_t _binary_Test_txt_end[];

/* ASCII advance widths (1/16 px), copied verbatim from the lv_font_cn_16
 * glyph_dsc table so the reader's line breaks match LVGL rendering. Indexed
 * by the ASCII code point (0x20..0x7E); the rest are left at 0. */
static const uint16_t s_ascii_w16[128] = {
    [0x20] = 57, 80, 117, 141, 141, 234, 172, 69, 85, 85, 118, 141, 69, 88, 69, 100,
    [0x30] = 141, 141, 141, 141, 141, 141, 141, 141, 141, 141, 69, 69, 141, 141, 141, 120,
    [0x40] = 239, 154, 167, 162, 175, 150, 140, 175, 185, 73, 136, 164, 137, 206, 184, 189,
    [0x50] = 160, 189, 161, 151, 152, 183, 145, 223, 144, 134, 154, 85, 100, 85, 141, 143,
    [0x60] = 154, 143, 157, 130, 157, 141, 81, 143, 154, 69, 69, 139, 71, 236, 155, 154,
    [0x70] = 157, 157, 97, 119, 95, 154, 131, 203, 124, 131, 120, 85, 68, 85, 141,
};

/* Real advances (1/16 px) of the rare typographic glyphs that lv_font_cn_16
 * carries with a non-fullwidth metric (docs/ebook.md 4.2). CJK, hiragana and
 * every FF00/3000-range glyph are exactly 256, so those fall back to
 * EBOOK_CHAR_W16; only these codepoints (drawn from the font's glyph_dsc)
 * deviate, and honoring their true advance keeps reader line breaks identical
 * to LVGL rendering. */
typedef struct { uint16_t cp; uint16_t w16; } typ_w16_t;
static const typ_w16_t s_typ_w16[] = {
    { 0x2011, 88 }, { 0x2012, 137 }, { 0x2013, 137 }, { 0x2014, 228 },
    { 0x201A, 69 }, { 0x201E, 117 }, { 0x2032, 69 },  { 0x2033, 117 },
    { 0x2039, 76 }, { 0x203A, 76 },  { 0x203C, 154 }, { 0x2047, 230 },
    { 0x2048, 191 }, { 0x2049, 191 }, { 0x2E3A, 428 }, { 0x2E3B, 628 },
};

/* True: *out receives the real advance of `cp` (may be 0). False: not in the
 * table, caller uses the 256 full-width default. */
static bool typ_adv_w(uint32_t cp, uint32_t *out)
{
    for (size_t i = 0; i < sizeof(s_typ_w16) / sizeof(s_typ_w16[0]); i++) {
        if (s_typ_w16[i].cp == cp) {
            *out = s_typ_w16[i].w16;
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * Line-break model: keep ONLY the source file's own line breaks.
 *
 * There is no character-class / punctuation / word rule layer at all. A display
 * line ends only when:
 *   1. the source text contains a newline (\n / \r / CRLF) — preserved verbatim;
 *   2. the next glyph would overflow the line width — then we hard-break right
 *      before it, so each line is filled to the brim.
 * Nothing else influences breaking. Deterministic: identical input always
 * yields identical output, so page flips and the background total-page count
 * agree.
 * ------------------------------------------------------------------------- */

/* Half-width form of a line-ending CJK punctuation (标点压缩, docs/ebook.md
 * 4.2). The lv_font_cn_16 font carries no narrow CJK punctuation, so the narrow
 * ASCII look-alike is substituted: its advance (s_ascii_w16) is smaller than the
 * 256/16 px full-width advance, freeing pixels at the line end. Returns 0 when
 * no usable narrow form exists (e.g. 0x2026 …); in that case the glyph is drawn
 * at its true advance. */
static uint8_t compress_punct(uint32_t cp)
{
    switch (cp) {
    case 0x3002: return '.';             /* 。 */
    case 0x3001: return ',';             /* 、 */
    case 0xFF01: return '!';             /* ！ */
    case 0xFF1F: return '?';             /* ？ */
    case 0xFF0C: return ',';             /* ， */
    case 0xFF1B: return ';';             /* ； */
    case 0xFF1A: return ':';             /* ： */
    case 0xFF09: return ')';             /* ） */
    case 0x3009: return '>';             /* 〉 */
    case 0x300B: return '>';             /* 》 */
    case 0x300D: return ']';             /* 」 */
    case 0x300F: return ']';             /* 』 */
    case 0x3011: return ']';             /* 】 */
    case 0x201D: return '"';             /* ” */
    case 0x2019: return '\'';            /* ’ */
    default:
        return 0;
    }
}

/* Windowed byte reader: serves bytes by absolute file offset through a
 * 4KB chunk window. SD files and the embedded ROM book share this. */
typedef struct {
    bool embedded;
    FILE *fp;
    const uint8_t *rom;
    size_t rom_size;
    uint8_t *buf;                        /* chunk window */
    size_t buf_cap;
    size_t base;                         /* file offset of buf[0] */
    size_t len;                          /* valid bytes in buf */
} reader_t;

/* Description of the currently open book (shared with the count task). */
typedef struct {
    bool embedded;
    const uint8_t *rom_start;
    size_t rom_size;
    char path[192];
    size_t size;
} book_src_t;

static uint8_t *s_chunk_a;               /* main-reader window */
static uint8_t *s_chunk_b;               /* count-task window */

static reader_t s_reader;
static book_src_t s_book;

static char s_page_buf[EBOOK_PAGE_BUF] EXT_RAM_BSS_ATTR;
static char s_scan_buf[2][EBOOK_LIST_MAX][EBOOK_NAME_MAX] EXT_RAM_BSS_ATTR;

static size_t s_cur_start;               /* start offset of the current page */
static size_t s_next_start;              /* start offset of the next page */
static size_t s_hist[EBOOK_HIST_N];      /* page-start history ring */
static int s_hist_head;                  /* oldest entry */
static int s_hist_count;
static int s_page;                       /* 1-based */
static bool s_is_open;

static uint32_t s_page_count;            /* written by the count task */
static uint32_t s_count_ver;
static uint32_t s_open_gen;              /* bump on open/close: invalidate counts */

static int s_scan_active;                /* index of the published list */
static int s_scan_count;
static bool s_scan_busy;
static uint32_t s_scan_ver;

static TaskHandle_t s_scan_task;
static TaskHandle_t s_count_task;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* --- byte window --- */

static int reader_byte(reader_t *r, size_t off)
{
    if (r->embedded) {
        return off < r->rom_size ? r->rom[off] : -1;
    }
    if (off < r->base || off >= r->base + r->len) {
        if (fseeko(r->fp, (off_t)off, SEEK_SET) != 0) {
            return -1;
        }
        r->base = off;
        r->len = fread(r->buf, 1, r->buf_cap, r->fp);
        if (r->len == 0) {
            return -1;
        }
    }
    return r->buf[off - r->base];
}

/* Decode one UTF-8 sequence at `off`; raw[] receives the source bytes (for
 * pass-through of invalid sequences), *len the byte count. Returns the code
 * point, or -1 at end of source. Invalid/truncated sequences are passed
 * through as their single lead byte. */
static int32_t reader_char(reader_t *r, size_t off, uint8_t raw[4], int *len)
{
    int b0 = reader_byte(r, off);
    if (b0 < 0) {
        return -1;
    }
    raw[0] = (uint8_t)b0;
    if (b0 < 0x80) {
        *len = 1;
        return b0;
    }
    int n;
    uint32_t cp;
    if ((b0 & 0xE0) == 0xC0) {
        n = 2;
        cp = (uint32_t)(b0 & 0x1F);
    }
    else if ((b0 & 0xF0) == 0xE0) {
        n = 3;
        cp = (uint32_t)(b0 & 0x0F);
    }
    else if ((b0 & 0xF8) == 0xF0) {
        n = 4;
        cp = (uint32_t)(b0 & 0x07);
    }
    else {
        *len = 1;
        return b0;                       /* invalid lead byte */
    }
    for (int i = 1; i < n; i++) {
        int b = reader_byte(r, off + (size_t)i);
        if (b < 0 || (b & 0xC0) != 0x80) {
            *len = 1;
            return b0;                   /* truncated: pass the lead byte */
        }
        raw[i] = (uint8_t)b;
        cp = (cp << 6) | (uint32_t)(b & 0x3F);
    }
    if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) ||
        (n == 4 && cp < 0x10000) || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF)) {
        *len = 1;
        return b0;                       /* overlong / surrogate / too large */
    }
    *len = n;
    return (int32_t)cp;
}

/* --- page layout (deterministic pagination engine) --- */

static void page_putc(char *out, size_t *out_len, size_t cap, char c)
{
    if (out && *out_len < cap - 1) {
        out[(*out_len)++] = c;
    }
}

static void page_put_raw(char *out, size_t *out_len, size_t cap,
                         const uint8_t *raw, int len)
{
    for (int i = 0; i < len; i++) {
        page_putc(out, out_len, cap, (char)raw[i]);
    }
}

static void line_end(char *out, size_t *out_len, size_t cap,
                     int *line, uint32_t *w16)
{
    if (*line < EBOOK_PAGE_LINES - 1) {
        page_putc(out, out_len, cap, '\n');
    }
    (*line)++;
    *w16 = 0;
}

/* Width (1/16 px) of one code point, honoring 标点压缩: a compressible
 * full-width punctuation is measured at its narrow ASCII advance. CJK and the
 * bulk of full-width glyphs use EBOOK_CHAR_W16. The exact advances of the rare
 * typographic glyphs come from s_typ_w16. */
static uint32_t cp_width(uint32_t cp)
{
    if (cp < 0x80) {
        /* Outside the printable ASCII range (control chars etc.) measure as a
         * space so they don't corrupt line-fill accounting. */
        return (cp >= 0x20) ? s_ascii_w16[cp] : s_ascii_w16[0x20];
    }
    uint8_t sub = compress_punct(cp);
    if (sub) {
        return s_ascii_w16[sub];
    }
    uint32_t w;
    if (typ_adv_w(cp, &w)) {
        return w;
    }
    return EBOOK_CHAR_W16;
}

/* Skip one newline sequence (LF, or CR / CRLF) at `off`. Returns the offset
 * just past it. */
static size_t skip_newline(reader_t *r, size_t off)
{
    uint8_t raw[4];
    int len;
    int32_t c = reader_char(r, off, raw, &len);
    if (c == '\n') {
        return off + (size_t)len;
    }
    if (c == '\r') {
        size_t n = off + (size_t)len;
        int32_t d = reader_char(r, n, raw, &len);
        if (d == '\n') {
            return n + (size_t)len;
        }
        return n;
    }
    return off;
}

/* --- greedy line-fill engine (deterministic pagination) ---
 *
 * Pipeline:  scan glyphs  ->  emit  ->  break on newline OR width overflow.
 *
 * fill_line() walks the source from *po, measuring each glyph with cp_width()
 * and emitting it. A line stops at the EARLIEST of:
 *   - a source newline (\n / \r / CRLF): preserved verbatim as a hard break;
 *   - the next glyph overflowing EBOOK_LINE_W16: a hard break is taken right
 *     before it, so the line is filled to the brim.
 * No other rule exists: no word integrity, no punctuation guard, no "--"
 * handling, no soft-line merging. Deterministic pagination follows. */

/* Fill one display line starting at *po. Emits glyphs into out[] and stops at
 * the first source newline or when the next glyph would overflow the line
 * width. On return:
 *   *po     advanced past the consumed text (points at the next line's start,
 *           i.e. just past a consumed newline, or at the overflowing glyph),
 *   *eof    set if the source ran out,
 *   *nl     set true if the line ended because of a source newline (not EOF or
 *           width overflow).
 * Returns true if a line was produced (the caller should emit a line
 * terminator unless this was the last page line). */
static bool fill_line(reader_t *r, size_t *po, char *out, size_t *out_len,
                      size_t cap, bool *eof, bool *nl)
{
    uint32_t w16 = 0;
    bool eof_l = false;
    bool nl_l = false;

    for (;;) {
        size_t cur = *po;
        uint8_t raw[4];
        int clen;
        int32_t cp = reader_char(r, cur, raw, &clen);
        if (cp < 0) {
            eof_l = true;
            break;
        }

        /* Source newline: preserve it as a hard line break. */
        if (cp == '\n' || cp == '\r') {
            *po = skip_newline(r, cur);
            nl_l = true;
            break;
        }

        /* Normalize the emitted code point (tab -> space). */
        int32_t emit_cp = cp;
        uint8_t emit_raw[4];
        int emit_len = clen;
        if (cp == '\t') {
            emit_cp = ' ';
            emit_raw[0] = ' ';
            emit_len = 1;
        } else {
            for (int i = 0; i < clen; i++) {
                emit_raw[i] = raw[i];
            }
        }

        uint32_t cw = cp_width((uint32_t)emit_cp);

        /* Width overflow: hard-break BEFORE this glyph so the line is full. */
        if (w16 > 0 && w16 + cw > EBOOK_LINE_W16) {
            break;
        }

        /* Never start a line with a space (a wrapped line that began with one). */
        if (w16 == 0 && emit_cp == ' ') {
            *po = cur + (size_t)clen;
            continue;
        }

        /* Emit. */
        uint8_t sub = (emit_cp >= 0x80) ? compress_punct((uint32_t)emit_cp) : 0;
        if (sub) {
            page_putc(out, out_len, cap, (char)sub);
        } else if (emit_cp < 0x80) {
            page_putc(out, out_len, cap, (char)emit_cp);
        } else {
            page_put_raw(out, out_len, cap, emit_raw, emit_len);
        }
        w16 += cw;
        *po = cur + (size_t)clen;
    }

    *eof = eof_l;
    *nl = nl_l;
    return true;
}

/* Lay out one page of exactly EBOOK_PAGE_LINES lines starting at byte offset
 * `start`. Writes the page text into `out` (NUL-terminated) when non-NULL.
 * Returns the byte offset where the next page starts (== file size at EOF). */
static size_t layout_page(reader_t *r, size_t start, char *out, size_t cap)
{
    size_t o = start;
    size_t out_len = 0;
    int line = 0;
    bool eof = false;
    uint32_t w16 = 0;                     /* scratch line width (cleared by helpers) */

    if (o == 0 && reader_byte(r, 0) == 0xEF &&
        reader_byte(r, 1) == 0xBB && reader_byte(r, 2) == 0xBF) {
        o = 3;                           /* strip UTF-8 BOM */
    }

    while (line < EBOOK_PAGE_LINES && !eof) {
        bool line_eof = false;
        bool line_nl = false;
        fill_line(r, &o, out, &out_len, cap, &line_eof, &line_nl);
        eof = line_eof;

        /* Trim a dangling trailing space (a wrapped line is never started with
         * one, but a source line may end with spaces). */
        while (out_len > 0 && out[out_len - 1] == ' ') {
            out_len--;
        }

        if (!eof) {
            line_end(out, &out_len, cap, &line, &w16);
        } else {
            /* Ran out of text: this is the final line, stop. */
            break;
        }
    }
    if (out && out_len > 0 && out[out_len - 1] == '\n') {
        out_len--;
    }
    if (out) {
        out[out_len] = '\0';
    }
    return o;
}

/* --- page-start history ring (backward navigation) --- */

static void hist_push(size_t off)
{
    if (s_hist_count == EBOOK_HIST_N) {  /* ring full: drop the oldest */
        s_hist_head = (s_hist_head + 1) % EBOOK_HIST_N;
        s_hist_count--;
    }
    s_hist[(s_hist_head + s_hist_count) % EBOOK_HIST_N] = off;
    s_hist_count++;
}

static bool hist_pop(size_t *off)
{
    if (s_hist_count == 0) {
        return false;
    }
    s_hist_count--;
    *off = s_hist[(s_hist_head + s_hist_count) % EBOOK_HIST_N];
    return true;
}

/* Deep-backward fallback: sequentially re-lay out the whole book from page 1
 * and rebuild the history of page-start offsets up to the current page.
 * One-time O(pages) cost, afterwards normal O(1) pops resume. */
static void rebuild_history(void)
{
    s_hist_count = 0;
    s_hist_head = 0;
    size_t off = 0;
    while (off != s_cur_start) {
        if (s_hist_count == EBOOK_HIST_N) {
            s_hist_head = (s_hist_head + 1) % EBOOK_HIST_N;
            s_hist_count--;
        }
        s_hist[(s_hist_head + s_hist_count) % EBOOK_HIST_N] = off;
        s_hist_count++;
        off = layout_page(&s_reader, off, NULL, 0);
        if (off >= s_book.size) {
            break;                       /* safety: current start unreachable */
        }
    }
}

/* --- SD scan --- */

static int name_cmp(const void *a, const void *b)
{
    return strcasecmp((const char *)a, (const char *)b);
}

static int scan_dir(char names[][EBOOK_NAME_MAX], int max)
{
    int n = 0;
    DIR *d = opendir("/sdcard");
    if (d != NULL) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL && n < max) {
            const char *fn = e->d_name;
            int len = (int)strlen(fn);
            if (len > 4 && strcasecmp(fn + len - 4, ".txt") == 0 &&
                e->d_type != DT_DIR) {
                strncpy(names[n], fn, EBOOK_NAME_MAX - 1);
                names[n][EBOOK_NAME_MAX - 1] = '\0';
                n++;
            }
        }
        closedir(d);
    }
    qsort(names, (size_t)n, EBOOK_NAME_MAX, name_cmp);
    return n;
}

static void ebook_scan_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int active;
        portENTER_CRITICAL(&s_mux);
        active = s_scan_active;
        portEXIT_CRITICAL(&s_mux);
        int work = 1 - active;           /* fill the inactive buffer */
        int n = scan_dir(s_scan_buf[work], EBOOK_LIST_MAX - 1);
        if (n < EBOOK_LIST_MAX - 1) {    /* ROM test book always last */
            strncpy(s_scan_buf[work][n], EBOOK_ROM_PREFIX " Test.txt",
                    EBOOK_NAME_MAX - 1);
            s_scan_buf[work][n][EBOOK_NAME_MAX - 1] = '\0';
            n++;
        }
        portENTER_CRITICAL(&s_mux);
        s_scan_active = work;
        s_scan_count = n;
        s_scan_busy = false;
        s_scan_ver++;
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGI(TAG, "scan done: %d book(s)", n);
    }
}

/* --- background total-page count --- */

static void ebook_count_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        book_src_t src;
        uint32_t gen;
        portENTER_CRITICAL(&s_mux);
        src = s_book;
        gen = s_open_gen;
        portEXIT_CRITICAL(&s_mux);

        if (src.size == 0 || (!src.embedded && src.path[0] == '\0')) {
            continue;
        }

        reader_t r;
        memset(&r, 0, sizeof(r));
        r.embedded = src.embedded;
        r.rom = src.rom_start;
        r.rom_size = src.rom_size;
        r.buf = s_chunk_b;
        r.buf_cap = EBOOK_CHUNK;
        if (!src.embedded) {
            r.fp = fopen(src.path, "rb");
            if (r.fp == NULL) {
                portENTER_CRITICAL(&s_mux);
                if (gen == s_open_gen) {
                    s_page_count = 0;
                    s_count_ver++;
                }
                portEXIT_CRITICAL(&s_mux);
                continue;
            }
        }

        uint32_t pages = 0;
        size_t off = 0;
        while (off < src.size) {
            off = layout_page(&r, off, NULL, 0);
            pages++;
        }
        if (r.fp != NULL) {
            fclose(r.fp);
        }
        ESP_LOGI(TAG, "count done: %u page(s)", (unsigned)pages);

        portENTER_CRITICAL(&s_mux);
        if (gen == s_open_gen && s_is_open) {
            s_page_count = pages;
            s_count_ver++;
        }
        portEXIT_CRITICAL(&s_mux);
    }
}

/* --- public API --- */

void ebook_init(void)
{
    s_chunk_a = heap_caps_malloc(EBOOK_CHUNK, MALLOC_CAP_SPIRAM);
    if (s_chunk_a == NULL) {
        s_chunk_a = malloc(EBOOK_CHUNK); /* fall back to internal RAM */
    }
    s_chunk_b = heap_caps_malloc(EBOOK_CHUNK, MALLOC_CAP_SPIRAM);
    if (s_chunk_b == NULL) {
        s_chunk_b = malloc(EBOOK_CHUNK);
    }
    s_reader.buf = s_chunk_a;
    s_reader.buf_cap = EBOOK_CHUNK;
    xTaskCreate(ebook_scan_task, "eb_scan", 4 * 1024, NULL, 4, &s_scan_task);
    xTaskCreate(ebook_count_task, "eb_count", 4 * 1024, NULL, 4, &s_count_task);
}

void ebook_scan_start(void)
{
    portENTER_CRITICAL(&s_mux);
    if (s_scan_busy) {
        portEXIT_CRITICAL(&s_mux);
        return;
    }
    s_scan_busy = true;
    portEXIT_CRITICAL(&s_mux);
    if (s_scan_task != NULL) {
        xTaskNotifyGive(s_scan_task);
    }
}

bool ebook_scan_busy(void)
{
    bool b;
    portENTER_CRITICAL(&s_mux);
    b = s_scan_busy;
    portEXIT_CRITICAL(&s_mux);
    return b;
}

uint32_t ebook_scan_version(void)
{
    uint32_t v;
    portENTER_CRITICAL(&s_mux);
    v = s_scan_ver;
    portEXIT_CRITICAL(&s_mux);
    return v;
}

int ebook_scan_count(void)
{
    int c;
    portENTER_CRITICAL(&s_mux);
    c = s_scan_count;
    portEXIT_CRITICAL(&s_mux);
    return c;
}

const char *ebook_scan_name(int idx)
{
    const char *name = "";
    portENTER_CRITICAL(&s_mux);
    if (idx >= 0 && idx < s_scan_count) {
        name = s_scan_buf[s_scan_active][idx];
    }
    portEXIT_CRITICAL(&s_mux);
    return name;
}

void ebook_close(void)
{
    if (s_reader.fp != NULL) {
        fclose(s_reader.fp);
        s_reader.fp = NULL;
    }
    /* s_book / s_open_gen / s_is_open are copied by the count task under the
     * mutex; publish the reset under the same lock so it never observes a
     * torn struct. Everything below is UI-task-private. */
    portENTER_CRITICAL(&s_mux);
    memset(&s_book, 0, sizeof(s_book));
    s_is_open = false;
    s_open_gen++;
    s_page = 0;
    s_page_count = 0;
    portEXIT_CRITICAL(&s_mux);
    s_cur_start = s_next_start = 0;
    s_hist_count = s_hist_head = 0;
    s_page_buf[0] = '\0';
}

bool ebook_open(int idx)
{
    char name[EBOOK_NAME_MAX];
    portENTER_CRITICAL(&s_mux);
    if (idx < 0 || idx >= s_scan_count) {
        portEXIT_CRITICAL(&s_mux);
        return false;
    }
    strncpy(name, s_scan_buf[s_scan_active][idx], EBOOK_NAME_MAX - 1);
    name[EBOOK_NAME_MAX - 1] = '\0';
    portEXIT_CRITICAL(&s_mux);

    ebook_close();

    /* Build the source description in a local first, then publish it under
     * the mutex in one go: the count task copies s_book atomically, so a
     * torn struct (e.g. a garbage src.size) could otherwise send its
     * page-count loop spinning forever at EOF. */
    book_src_t src;
    memset(&src, 0, sizeof(src));

    const bool embedded =
        (strncmp(name, EBOOK_ROM_PREFIX, strlen(EBOOK_ROM_PREFIX)) == 0);
    if (embedded) {
        src.embedded = true;
        src.rom_start = _binary_Test_txt_start;
        src.rom_size = (size_t)(_binary_Test_txt_end -
                                _binary_Test_txt_start);
        src.size = src.rom_size;
    }
    else {
        snprintf(src.path, sizeof(src.path), "/sdcard/%s", name);
        FILE *fp = fopen(src.path, "rb");
        if (fp == NULL) {
            ESP_LOGE(TAG, "open '%s' failed", src.path);
            ebook_close();
            return false;
        }
        fseeko(fp, 0, SEEK_END);
        src.size = (size_t)ftello(fp);
        fseeko(fp, 0, SEEK_SET);
        s_reader.fp = fp;                /* reader is UI-task-private */
    }
    s_reader.embedded = embedded;
    s_reader.rom = src.rom_start;
    s_reader.rom_size = src.rom_size;
    s_reader.base = 0;
    s_reader.len = 0;

    portENTER_CRITICAL(&s_mux);
    s_book = src;
    s_is_open = true;
    s_open_gen++;                        /* start the count fresh for this book */
    portEXIT_CRITICAL(&s_mux);

    s_cur_start = 0;
    s_page = 1;
    s_next_start = layout_page(&s_reader, 0, s_page_buf, sizeof(s_page_buf));
    if (s_count_task != NULL) {
        xTaskNotifyGive(s_count_task);
    }
    ESP_LOGI(TAG, "open '%s' embedded=%d size=%u", name, embedded ? 1 : 0,
             (unsigned)src.size);
    return true;
}

int ebook_page(void)
{
    return s_page;
}

int ebook_page_count(void)
{
    int c;
    portENTER_CRITICAL(&s_mux);
    c = (int)s_page_count;
    portEXIT_CRITICAL(&s_mux);
    return c;
}

uint32_t ebook_count_version(void)
{
    uint32_t v;
    portENTER_CRITICAL(&s_mux);
    v = s_count_ver;
    portEXIT_CRITICAL(&s_mux);
    return v;
}

int ebook_percent(void)
{
    if (s_book.size == 0) {
        return 0;
    }
    uint64_t pct = (uint64_t)s_next_start * 100 / s_book.size;
    return (int)(pct > 100 ? 100 : pct);
}

bool ebook_at_start(void)
{
    return s_page <= 1;
}

bool ebook_at_end(void)
{
    return s_next_start >= s_book.size;
}

void ebook_page_flip(int dir)
{
    if (!s_is_open) {
        return;
    }
    if (dir > 0) {
        if (s_next_start >= s_book.size) {
            return;                      /* last page */
        }
        const size_t prev_start = s_cur_start;
        const size_t prev_next = s_next_start;
        hist_push(prev_start);
        s_cur_start = s_next_start;
        s_next_start = layout_page(&s_reader, s_cur_start,
                                   s_page_buf, sizeof(s_page_buf));
        s_page++;
        if (s_page_buf[0] == '\0' && s_next_start >= s_book.size) {
            /* Blank page at EOF (file ends with a dangling newline): undo. */
            size_t prev;
            hist_pop(&prev);
            s_cur_start = prev_start;
            s_next_start = prev_next;
            s_page--;
        }
    }
    else {
        if (s_page <= 1) {
            return;                      /* first page */
        }
        size_t prev;
        if (!hist_pop(&prev)) {
            rebuild_history();
            if (!hist_pop(&prev)) {
                return;
            }
        }
        s_cur_start = prev;
        s_next_start = layout_page(&s_reader, s_cur_start,
                                   s_page_buf, sizeof(s_page_buf));
        s_page--;
    }
}

const char *ebook_page_text(void)
{
    return s_page_buf;
}
