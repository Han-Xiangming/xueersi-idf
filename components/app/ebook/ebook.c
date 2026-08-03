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
#define EBOOK_PAGE_LINES    5            /* lines per page */
#define EBOOK_LINE_W        152          /* text-area width, px */
#define EBOOK_LINE_W16      (EBOOK_LINE_W * 16)  /* width in 1/16 px */
#define EBOOK_CHAR_W16      192          /* fullwidth advance, 1/16 px = 12 px */
#define EBOOK_HIST_N        32           /* page-start offset history ring */
#define EBOOK_ROM_PREFIX    "(ROM)"

/* Built-in test book compiled into the firmware via EMBED_FILES
 * (IDF names the symbols from the file basename only). */
extern const uint8_t _binary_Test_txt_start[];
extern const uint8_t _binary_Test_txt_end[];

/* ASCII advance widths (1/16 px), copied verbatim from the lv_font_cn_12
 * glyph_dsc table so the reader's line breaks match LVGL rendering. */
static const uint16_t s_ascii_w16[128] = {
    /* 0x20 */ 43, 60, 88, 105, 105, 175, 129, 52, 64, 64, 88, 105, 52, 66, 52, 75,
    /* 0x30 */ 105, 105, 105, 105, 105, 105, 105, 105, 105, 105, 52, 52, 105, 105, 105, 90,
    /* 0x40 */ 180, 116, 125, 122, 131, 112, 105, 131, 139, 55, 102, 123, 103, 154, 138, 142,
    /* 0x50 */ 120, 142, 120, 113, 114, 137, 109, 167, 108, 100, 115, 64, 75, 64, 105, 107,
    /* 0x60 */ 116, 107, 118, 97, 118, 105, 61, 107, 116, 52, 52, 104, 54, 177, 116, 116,
    /* 0x70 */ 118, 118, 73, 89, 71, 116, 98, 152, 93, 98, 90, 64, 51, 64, 105,
};

/* Real advances (1/16 px) of the rare typographic glyphs that lv_font_cn_12
 * carries with a non-fullwidth metric (docs/ebook.md 4.2). CJK, hiragana and
 * every FF00/3000-range glyph are exactly 192, so those fall back to
 * EBOOK_CHAR_W16; only these codepoints (drawn from the font's glyph_dsc)
 * deviate, and honoring their true advance keeps reader line breaks identical
 * to LVGL rendering. */
typedef struct { uint16_t cp; uint16_t w16; } typ_w16_t;
static const typ_w16_t s_typ_w16[] = {
    { 0x2011, 66 }, { 0x2012, 103 }, { 0x2013, 103 }, { 0x2014, 171 },
    { 0x2019, 52 }, { 0x201C, 88 },  { 0x2024, 52 },  { 0x2025, 88 },
    { 0x2027, 57 }, { 0x2028, 57 },  { 0x202A, 115 }, { 0x202C, 173 },
    { 0x202D, 143 }, { 0x202E, 143 }, { 0x202F, 321 }, { 0x2030, 471 },
    { 0x205B, 0 },  { 0x205C, 0 },   { 0x205D, 0 },   { 0x205E, 0 },
    { 0x205F, 48 }, { 0x2060, 48 },  { 0x20C7, 0 },   { 0x20C8, 0 },
};

/* True: *out receives the real advance of `cp` (may be 0). False: not in the
 * table, caller uses the 192 full-width default. */
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

/* Trailing punctuation that must not begin a line (docs/ebook.md 4.2). */
static bool is_trail_punct(uint32_t cp)
{
    switch (cp) {
    case 0x3002: case 0x3001: case 0xFF01: case 0xFF1F: case 0xFF0C:
    case 0xFF1B: case 0xFF1A: case 0xFF09: case 0x3009: case 0x300B:
    case 0x300D: case 0x300F: case 0x3011: case 0x2026: case 0x201D:
    case 0x2019: case 0x22: case 0x27: case 0x60: case 0x29:
    case 0x5D: case 0x7D:
        return true;
    default:
        return false;
    }
}

/* Half-width form of a line-ending punctuation (标点压缩, docs/ebook.md 4.2).
 * The lv_font_cn_12 font carries no narrow CJK punctuation, so the narrow
 * ASCII look-alike is substituted: its true advance (s_ascii_w16) is smaller
 * than the 192/16 px full-width advance, freeing 6-9 px at the line end.
 * Substitutes are guaranteed to exist in the font (0x20-0x7E). Returns 0 for
 * code points without a usable narrow form (e.g. 0x2026 …). */
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

/* Drop a dangling trailing space so that no line ends with one (docs/ebook.md
 * 4.3). *last_sp tracks whether the most recent emit was a space; when out is
 * NULL (page-count path) only the flag is cleared -- the popped advance affects
 * only the already-finished line, never following offsets. */
static void line_drop_space(char *out, size_t *out_len, uint32_t *w16,
                            bool *last_sp)
{
    if (!*last_sp) {
        return;
    }
    if (out) {
        while (*out_len > 0 && out[*out_len - 1] == ' ') {
            (*out_len)--;
            *w16 -= s_ascii_w16[' '];
        }
    }
    *last_sp = false;
}

/* Lay out one page of exactly EBOOK_PAGE_LINES lines starting at byte offset
 * `start`. Writes the page text into `out` (NUL-terminated) when non-NULL.
 * Returns the byte offset where the next page starts (== file size at EOF).
 * Deterministic: the same input always yields the same output, so page flips
 * and the background total-page count cannot diverge. */
static size_t layout_page(reader_t *r, size_t start, char *out, size_t cap)
{
    size_t o = start;
    size_t out_len = 0;
    int line = 0;
    uint32_t w16 = 0;
    bool eof = false;
    bool last_sp = false;                /* last emitted char was a space */

    if (o == 0 && reader_byte(r, 0) == 0xEF &&
        reader_byte(r, 1) == 0xBB && reader_byte(r, 2) == 0xBF) {
        o = 3;                           /* strip UTF-8 BOM */
    }

    while (line < EBOOK_PAGE_LINES) {
        uint8_t raw[4];
        int clen;
        size_t po = o;
        int32_t cp = reader_char(r, po, raw, &clen);
        if (cp < 0) {
            eof = true;
            break;
        }

        if (cp == '\r') {                /* CRLF -> LF */
            o = po + (size_t)clen;
            continue;
        }
        if (cp == '\t') {                /* tab -> space */
            raw[0] = ' ';
            clen = 1;
            cp = ' ';
        }

        /* 标点压缩: every compressible full-width punctuation is emitted in
         * its narrow ASCII form and measured at the ASCII advance, so lines
         * hold more characters (docs/ebook.md 4.2). */
        const uint8_t sub = compress_punct((uint32_t)cp);
        uint32_t cw;
        if (cp < 0x80) {
            cw = s_ascii_w16[cp];
        }
        else if (sub) {
            cw = s_ascii_w16[sub];
        }
        else if (!typ_adv_w((uint32_t)cp, &cw)) {
            cw = EBOOK_CHAR_W16;         /* CJK / 全角 = fixed 192/16 px */
        }

        if (cp == ' ') {
            if (w16 == 0) {              /* never begin a line with a space */
                o = po + (size_t)clen;
                continue;
            }
            if (w16 + cw > EBOOK_LINE_W16) {   /* dangling space: drop it */
                line_drop_space(out, &out_len, &w16, &last_sp);
                o = po + (size_t)clen;
                line_end(out, &out_len, cap, &line, &w16);
                continue;
            }
            page_putc(out, &out_len, cap, ' ');
            w16 += cw;
            last_sp = true;
            o = po + (size_t)clen;
            continue;
        }

        if (cp >= 0x21 && cp <= 0x7E) {
            /* ASCII run = unbreakable word (letters/digits and '-' '.' '/' etc);
             * breaking only between words keeps '--', route numbers and URLs
             * intact (docs/ebook.md 4.3). */
            size_t run_off = po;
            uint32_t run_w = 0;
            while (1) {
                int bo = reader_byte(r, run_off);
                if (bo < 0x21 || bo > 0x7E) {
                    break;
                }
                run_w += s_ascii_w16[bo];
                run_off++;
            }
            if (w16 > 0 && w16 + run_w > EBOOK_LINE_W16) {
                line_drop_space(out, &out_len, &w16, &last_sp);
                line_end(out, &out_len, cap, &line, &w16);
                continue;
            }
            while (po < run_off) {
                page_putc(out, &out_len, cap, (char)reader_byte(r, po));
                w16 += s_ascii_w16[reader_byte(r, po)];
                po++;
            }
            last_sp = false;
            o = run_off;
            continue;
        }

        if (cp != '\n' && w16 > 0 && w16 + cw > EBOOK_LINE_W16) {
            if (sub) {
                /* Trailing punctuation must not begin a line: absorb it (it
                 * is already in its narrow form) and break. */
                page_putc(out, &out_len, cap, (char)sub);
                w16 += cw;
                last_sp = false;
                o = po + (size_t)clen;
                line_end(out, &out_len, cap, &line, &w16);
                continue;
            }
            if (is_trail_punct((uint32_t)cp)) {
                /* No narrow form (e.g. 0x2026 …): absorb full-width as
                 * before (may spill a few px; the bitmap of these glyphs is
                 * narrow, so clipping does not eat it). */
                page_put_raw(out, &out_len, cap, raw, clen);
                w16 += cw;
                last_sp = false;
                o = po + (size_t)clen;
                line_end(out, &out_len, cap, &line, &w16);
                continue;
            }
            line_drop_space(out, &out_len, &w16, &last_sp);
            line_end(out, &out_len, cap, &line, &w16);   /* hard break */
        }

        if (cp == '\n') {
            line_drop_space(out, &out_len, &w16, &last_sp);
            o = po + (size_t)clen;
            line_end(out, &out_len, cap, &line, &w16);
            continue;
        }

        if (sub) {
            page_putc(out, &out_len, cap, (char)sub);
        }
        else {
            page_put_raw(out, &out_len, cap, raw, clen);
        }
        w16 += cw;
        last_sp = false;
        o = po + (size_t)clen;
    }

    /* Last page: a dangling trailing '\n' (empty final line) is dropped,
     * and trailing spaces are trimmed off the final line. */
    if (out && eof && out_len > 0) {
        if (out[out_len - 1] == '\n') {
            out_len--;
        }
        while (out_len > 0 && out[out_len - 1] == ' ') {
            out_len--;
        }
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
    memset(&s_book, 0, sizeof(s_book));
    s_is_open = false;
    s_open_gen++;
    s_page = 0;
    s_page_count = 0;
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

    const bool embedded =
        (strncmp(name, EBOOK_ROM_PREFIX, strlen(EBOOK_ROM_PREFIX)) == 0);
    memset(&s_book, 0, sizeof(s_book));
    if (embedded) {
        s_book.embedded = true;
        s_book.rom_start = _binary_Test_txt_start;
        s_book.rom_size = (size_t)(_binary_Test_txt_end -
                                   _binary_Test_txt_start);
        s_book.size = s_book.rom_size;
    }
    else {
        snprintf(s_book.path, sizeof(s_book.path), "/sdcard/%s", name);
        s_reader.fp = fopen(s_book.path, "rb");
        if (s_reader.fp == NULL) {
            ESP_LOGE(TAG, "open '%s' failed", s_book.path);
            ebook_close();
            return false;
        }
        fseeko(s_reader.fp, 0, SEEK_END);
        s_book.size = (size_t)ftello(s_reader.fp);
        fseeko(s_reader.fp, 0, SEEK_SET);
    }
    s_reader.embedded = embedded;
    s_reader.rom = s_book.rom_start;
    s_reader.rom_size = s_book.rom_size;
    s_reader.base = 0;
    s_reader.len = 0;

    s_is_open = true;
    s_open_gen++;                        /* start the count fresh for this book */
    s_cur_start = 0;
    s_page = 1;
    s_next_start = layout_page(&s_reader, 0, s_page_buf, sizeof(s_page_buf));
    if (s_count_task != NULL) {
        xTaskNotifyGive(s_count_task);
    }
    ESP_LOGI(TAG, "open '%s' embedded=%d size=%u", name, embedded ? 1 : 0,
             (unsigned)s_book.size);
    return true;
}

bool ebook_is_open(void)
{
    return s_is_open;
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
