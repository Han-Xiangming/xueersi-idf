/*
 * Application layer: TXT ebook reader.
 * See app/ebook.h and docs/ebook.md.
 */
#define LOG_LOCAL_LEVEL ESP_LOG_INFO
#include "ebook.h"
#include "ebook_progress.h"

#include <dirent.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"

static const char *TAG = "ebook";

/* --- Tuning constants (docs/ebook.md) --- */
#define EBOOK_NAME_MAX      64           /* book name buffer size */
#define EBOOK_PATH_MAX      320          /* book path buffer size (matches player) */
#define EBOOK_LIST_MAX      512          /* max books in the scan list */
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

/* Page-start offset table built by the count task: entry k is the byte
 * offset where the canonical page (from offset 0) k starts. Pagination is
 * deterministic, so these are exactly the UI reader's page starts for any
 * session that never jumped. rebuild_history() uses the table to restore
 * backward history in O(1) instead of re-laying out the whole book on the
 * UI task (multi-second freeze on large books). 64k entries * 4 B = 256 KB
 * PSRAM covers ~64k pages (~10-15 MB of CJK text); larger books fall back
 * to the slow synchronous relayout. */
#define EBOOK_START_TABLE_CAP (64 * 1024)

/* --- reading progress (on-card file) ------------------------------------
 * One file next to the books, so the progress moves with the card and
 * survives firmware updates.
 *
 * Format v2: a fixed-size array of EBOOK_PROG_MAX slots. A book is addressed
 * by a CONTENT fingerprint (file size + a head/tail sample), not by its path,
 * so renaming, moving or re-copying a book keeps its reading position. Each
 * slot carries its own CRC16 and a monotonic sequence number: saving one book
 * is a single positioned write of one slot, and a corrupt slot costs only
 * that one book. There is deliberately no global checksum — it would have to
 * be rewritten on every save, defeating the single-slot write.
 *
 *   layout: [16-byte header][slot 0] ... [slot EBOOK_PROG_MAX-1]
 *   a slot with fp == 0 is empty
 *
 * The legacy v1 file ("/.progress", a path-hash MRU table) is only consulted
 * as a one-time migration source: when a book has no v2 slot yet but a v1 match
 * (path hash + size), its offset seeds a v2 slot which restore_position() then
 * validates against v2's context bytes — so a book whose contents changed still
 * degrades instead of resuming to a wrong offset. Migrated v1 entries are
 * dropped; any leftovers are removed by the 清除进度 option.
 *
 * A debounced save runs EBOOK_SAVE_DELAY_MS after the last page flip, or
 * immediately once EBOOK_SAVE_FORCE_PAGES flips pile up;
 * ebook_close() / ebook_progress_flush() save synchronously.
 * --------------------------------------------------------------------- */
/* Reading-progress persistence (file format, NVS mirror, v1 migration) has
 * been extracted into components/ebook_progress. Its constants and types are
 * declared in ebook_progress.h, which is included above. */
#define EBOOK_SAVE_DELAY_MS 1500
#define EBOOK_SAVE_POLL_MS  250
#define EBOOK_SAVE_FORCE_PAGES 10        /* force a flush after N flips */
#define EBOOK_SAVE_RETRY_MS 2000         /* back-off after a failed write */

/* (v1 file constants moved to ebook_progress.h) */

/* eb_slot_t / eb_nvs_mirror_t / eb_v1_t are declared in ebook_progress.h. */

/* eb_nvs_mirror_t is declared in ebook_progress.h. */

/* eb_v1_t is declared in ebook_progress.h; its layout asserts live there. */

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
 * 4KB chunk window. */
typedef struct {
    FILE *fp;
    uint8_t *buf;                        /* chunk window */
    size_t buf_cap;
    size_t base;                         /* file offset of buf[0] */
    size_t len;                          /* valid bytes in buf */
} reader_t;

/* Description of the currently open book (shared with the count task). */
typedef struct {
    char path[EBOOK_PATH_MAX];
    size_t size;
} book_src_t;

static uint8_t *s_chunk_a;               /* main-reader window */
static uint8_t *s_chunk_b;               /* count-task window */
/* (fingerprint sample buffer moved into ebook_progress.c) */

static reader_t s_reader;
static book_src_t s_book;
/* Content fingerprint of the open book. Published under s_mux: the save task
 * compares it against its snapshot to detect a book switch. */
static uint64_t s_book_fp;

static char s_page_buf[EBOOK_PAGE_BUF] EXT_RAM_BSS_ATTR;
static char s_scan_buf[2][EBOOK_LIST_MAX][EBOOK_PATH_MAX] EXT_RAM_BSS_ATTR;

/* Page-start table (see EBOOK_START_TABLE_CAP). Written by the count task,
 * published under s_mux once the walk finishes; entries are immutable while
 * s_table_valid is true (the table is invalidated on open/close before any
 * writer can overwrite it). */
static uint32_t *s_start_table;
static uint32_t s_table_count;
static bool s_table_valid;

static size_t s_cur_start;               /* start offset of the current page */
static size_t s_next_start;              /* start offset of the next page */
/* Origin of the current page sequence: the resume offset at open or the
 * jump target. Page starts after a jump are laid out from this offset and
 * are NOT canonical (they do not coincide with the from-zero layout), so
 * backward history must be rebuilt from here. UI-task-private. */
static size_t s_session_start;
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
static bool s_scan_pending;              /* a load request landed mid-scan: redo */
static uint32_t s_scan_ver;

/* The directory the next/last scan walks. Defaults to the whole eBook tree
 * (EBOOK_ROOT). Set by ebook_scan_root(); read by ebook_scan_task. */
static char s_load_root[EBOOK_PATH_MAX] = EBOOK_ROOT;

/* Display name of the currently loaded source: "整卡" for the whole tree,
 * else the folder's basename. Mirrors the player's s_src_name. */
static char s_src_name[EBOOK_NAME_MAX];

static TaskHandle_t s_scan_task;
static TaskHandle_t s_count_task;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* --- reading-progress snapshot (UI task writes, save task reads) ---
 * The save task must not touch s_cur_start / s_page (UI-task-private), so
 * progress_arm() copies a snapshot under the mutex; progress_save() only
 * writes it out when the open book still matches s_snap_fp (a book switch
 * re-arms immediately, so the snapshot always belongs to the current book).
 * The on-card slot array is mirrored into s_prog[] on open; a save rewrites
 * exactly one slot in place. */
/* Position waiting to be written: the UI task fills it in under s_mux, the
 * save task copies it out in one go before touching the card. Type eb_snap_t
 * now lives in ebook_progress.h. */
static eb_snap_t s_snap;
/* Cross-task flags: the UI task (flip/open/close/flush) writes them, the
 * background save task reads them. Marked volatile so the save task re-loads
 * them from memory every loop on an SMP core instead of trusting a cached
 * register value. */
static volatile bool s_save_pending;
static volatile uint32_t s_save_after;  /* tick deadline for the debounce */
static int s_flip_since_save;            /* flips since the last save */
static bool s_save_failed;               /* sticky: the last save failed */
static uint32_t s_resume_pct;            /* restored position %, 0 = page 1 */
static TaskHandle_t s_save_task;

/* (reading-progress persistence state moved to components/ebook_progress) */

/* s_page is only an estimate until the count task publishes the page-start
 * table; page_calibrate() replaces it with the exact value. */
static bool s_page_calib;
static ebook_resume_t s_resume_kind;     /* how the position was restored */

/* --- byte window --- */

static int reader_byte(reader_t *r, size_t off)
{
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

/* Deep-backward fallback: rebuild the history ring of page-start offsets
 * before the current page. Afterwards normal O(1) pops resume.
 *
 * Fast path (the common case): the count task already laid the whole book
 * out from offset 0 and recorded every canonical page start in
 * s_start_table (deterministic pagination => identical to the UI reader's
 * pages). A canonical current page is then restored in O(1) instead of
 * re-laying the book out here — the old path blocked the UI task for
 * seconds on large books. */
static void rebuild_history(void)
{
    s_hist_count = 0;
    s_hist_head = 0;

    if (s_start_table != NULL) {
        uint32_t n;
        portENTER_CRITICAL(&s_mux);
        n = s_table_valid ? s_table_count : 0;
        portEXIT_CRITICAL(&s_mux);
        if (n > 0) {
            const uint32_t *t = s_start_table;
            uint32_t lo = 0, hi = n - 1, k = (uint32_t)-1;
            while (lo <= hi) {          /* k = last index with t[k] < cur */
                const uint32_t mid = (lo + hi) / 2;
                if (t[mid] < (uint32_t)s_cur_start) {
                    k = mid;
                    lo = mid + 1;
                } else {
                    hi = mid - 1;
                }
            }
            const bool canonical = k != (uint32_t)-1 && k + 1 < n &&
                                   t[k + 1] == (uint32_t)s_cur_start;
            if (canonical) {
                /* The user is on a canonical page: the whole ring is in the
                 * table. (Keeps the last EBOOK_HIST_N entries via hist_push.) */
                for (uint32_t i = 0; i <= k; i++) {
                    hist_push(t[i]);
                }
                return;
            }
            /* The current page belongs to a post-jump session whose starts
             * are laid out from s_session_start and are not canonical.
             * Restore the canonical part below the session start from the
             * table, then lay the session part out (bounded by the distance
             * read since the jump). */
            k = (uint32_t)-1;
            lo = 0;
            hi = n - 1;
            while (lo <= hi) {      /* k = last index with t[k] < session start */
                const uint32_t mid = (lo + hi) / 2;
                if (t[mid] < (uint32_t)s_session_start) {
                    k = mid;
                    lo = mid + 1;
                } else {
                    hi = mid - 1;
                }
            }
            if (k != (uint32_t)-1) {
                for (uint32_t i = 0; i <= k; i++) {
                    hist_push(t[i]);
                }
            }
            if (s_cur_start > s_session_start) {
                size_t off = s_session_start;
                while (off < s_cur_start) {
                    hist_push(off);
                    const size_t prev = off;
                    off = layout_page(&s_reader, off, NULL, 0);
                    if (off == prev) {
                        break;      /* read error: no forward progress */
                    }
                }
            }
            return;
        }
    }

    /* Fallback (no table yet, or allocation failed): walk the canonical
     * sequence from 0, then the session part from s_session_start. */
    {
        size_t off = 0;
        while (off < s_cur_start) {
            hist_push(off);
            const size_t prev = off;
            off = layout_page(&s_reader, off, NULL, 0);
            if (off == prev) {
                break;              /* read error: no forward progress */
            }
        }
        if (off != s_cur_start && s_cur_start > s_session_start) {
            size_t off2 = s_session_start;
            while (off2 < s_cur_start) {
                hist_push(off2);
                const size_t prev = off2;
                off2 = layout_page(&s_reader, off2, NULL, 0);
                if (off2 == prev) {
                    break;          /* read error: no forward progress */
                }
            }
        }
    }
}

/* --- SD scan --- */

/* Sort books by file name: shorter names first, then lexicographic by name,
 * then by full path (so same-named books in different folders keep a stable
 * order). The name is the last '/' component of the stored full path. */
static int name_cmp(const void *a, const void *b)
{
    const char *pa = (const char *)a;
    const char *pb = (const char *)b;
    const char *ba = strrchr(pa, '/');
    const char *bb = strrchr(pb, '/');
    ba = (ba != NULL) ? ba + 1 : pa;
    bb = (bb != NULL) ? bb + 1 : pb;
    size_t la = strlen(ba), lb = strlen(bb);
    if (la != lb) {
        return (la < lb) ? -1 : 1;
    }
    int c = strcasecmp(ba, bb);
    if (c != 0) {
        return c;
    }
    return strcasecmp(pa, pb);
}

/* Recursively collect .txt files under `dir` into `paths`, in-place. `depth`
 * bounds recursion so a pathological directory cycle can't overflow the
 * stack (mirrors the player's scan, components/app/player/player.c). Returns
 * the running total; stops early once `max` is reached. */
static int scan_dir(char paths[][EBOOK_PATH_MAX], int max, const char *dir,
                    int depth)
{
    if (depth > 16 || max <= 0) {
        return 0;
    }
    int n = 0;
    DIR *d = opendir(dir);
    if (d != NULL) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL && n < max) {
            const char *fn = e->d_name;
            if (fn[0] == '.') {              /* skip ".", "..", hidden */
                continue;
            }
            char child[EBOOK_PATH_MAX];
            snprintf(child, sizeof(child), "%s/%s", dir, fn);

            /* Decide file vs directory without relying on d_type (unreliable
             * on FATFS): stat the entry. A directory is recursed into; a
             * regular file ending in .txt is added with its full path. */
            struct stat st;
            if (stat(child, &st) != 0) {
                continue;
            }
            if (S_ISDIR(st.st_mode)) {
                n += scan_dir(paths + n, max - n, child, depth + 1);
            } else if (S_ISREG(st.st_mode)) {
                int len = (int)strlen(fn);
                if (len > 4 && strcasecmp(fn + len - 4, ".txt") == 0) {
                    snprintf(paths[n], EBOOK_PATH_MAX, "%s", child);
                    n++;
                }
            }
        }
        closedir(d);
    }
    return n;
}

static void ebook_scan_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        portENTER_CRITICAL(&s_mux);
        s_scan_busy = true;
        portEXIT_CRITICAL(&s_mux);
        do {
            portENTER_CRITICAL(&s_mux);
            s_scan_pending = false;
            portEXIT_CRITICAL(&s_mux);
            int active;
            portENTER_CRITICAL(&s_mux);
            active = s_scan_active;
            portEXIT_CRITICAL(&s_mux);
            int work = 1 - active;           /* fill the inactive buffer */
            int n = scan_dir(s_scan_buf[work], EBOOK_LIST_MAX,
                             s_load_root, 0);
            /* readdir/stat order is arbitrary across directories; sort so the
             * list is stable across rescans: file-name length first (shortest
             * at the top), then lexicographic by name (mirrors the player's
             * folder scan for stability of same-folder groups). */
            if (n > 1) {
                qsort(s_scan_buf[work], (size_t)n, EBOOK_PATH_MAX, name_cmp);
            }
            /* Source display name: "整卡" for the whole tree, else the folder
             * basename (mirrors player_load_folder). */
            const char *b = strrchr(s_load_root, '/');
            const char *base = (b != NULL) ? b + 1 : s_load_root;
            if (strcasecmp(base, "eBook") == 0) {
                snprintf(s_src_name, sizeof(s_src_name), "整卡");
            }
            else {
                size_t blen = strnlen(base, sizeof(s_src_name) - 1);
                memcpy(s_src_name, base, blen);
                s_src_name[blen] = '\0';
            }
            portENTER_CRITICAL(&s_mux);
            s_scan_active = work;
            s_scan_count = n;
            s_scan_ver++;
            portEXIT_CRITICAL(&s_mux);
            ESP_LOGI(TAG, "scan '%s' done: %d book(s)", s_load_root, n);
        } while (s_scan_pending);   /* a request landed mid-scan: redo */
        portENTER_CRITICAL(&s_mux);
        s_scan_busy = false;
        portEXIT_CRITICAL(&s_mux);
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

        if (src.size == 0 || src.path[0] == '\0') {
            continue;
        }

        reader_t r;
        memset(&r, 0, sizeof(r));
        r.buf = s_chunk_b;
        r.buf_cap = EBOOK_CHUNK;
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

        uint32_t pages = 0;
        size_t off = 0;
        for (;;) {
            const size_t start = off;      /* byte offset of page `pages` */
            if (off >= src.size) {
                break;                     /* normal end of walk */
            }
            off = layout_page(&r, off, NULL, 0);
            if (off == start) {
                /* No forward progress: a mid-file SD read error, or the file
                 * shrank after open (src.size is the open-time size). Treat
                 * it as EOF instead of spinning here forever at 100% CPU. */
                ESP_LOGW(TAG, "count aborted at offset %u (SD read error)",
                         (unsigned)start);
                break;
            }
            if (s_start_table != NULL && pages < EBOOK_START_TABLE_CAP) {
                s_start_table[pages] = (uint32_t)start;
            }
            pages++;
        }
        fclose(r.fp);
        ESP_LOGI(TAG, "count done: %u page(s)", (unsigned)pages);

        portENTER_CRITICAL(&s_mux);
        if (gen == s_open_gen && s_is_open) {
            s_page_count = pages;
            s_count_ver++;
            /* Publish the page-start table built on this pass. Every entry
             * written so far is a valid canonical page start (the entries
             * before an abort are untouched), so the table is usable even
             * after a read error — rebuild_history() falls back when the
             * current page is not covered. */
            if (s_start_table != NULL) {
                s_table_count = (pages < EBOOK_START_TABLE_CAP)
                                    ? pages : EBOOK_START_TABLE_CAP;
                s_table_valid = true;
            }
        }
        portEXIT_CRITICAL(&s_mux);
    }
}

/* --- reading progress (on-card file) --- */

/* 32-bit FNV-1a of the book path. Stored in every slot so the book list can
 * show a progress figure without opening the file. A collision can only cost
 * a missing percentage — never a wrong position, which the fingerprint
 * decides. */
/* Hashing (fnv1a32/64/48, crc16) and book_fingerprint() now live in
 * components/ebook_progress (see ebook_progress.h). */

/* Display name of a book: file name without the .txt extension, truncated on
 * a UTF-8 character boundary. Stored in the slot so a "recently read" list
 * needs no file access. */
static void make_name(char *dst, size_t dst_sz, const char *path)
{
    const char *b = strrchr(path, '/');
    b = (b != NULL) ? b + 1 : path;
    size_t n = strlen(b);
    if (n > 4 && strcasecmp(b + n - 4, ".txt") == 0) {
        n -= 4;
    }
    if (n >= dst_sz) {
        n = dst_sz - 1;
    }
    while (n > 0 && ((uint8_t)b[n] & 0xC0) == 0x80) {
        n--;                             /* never cut inside a UTF-8 sequence */
    }
    memcpy(dst, b, n);
    dst[n] = '\0';
}

/* --- v1 migration helpers (read-only, used once to seed v2) --- */

/* fnv1a48() and v1 migration helpers (progress_load_v1 / progress_find_v1)
 * now live in components/ebook_progress (see ebook_prog_v1_lookup). */

/* progress_drop_v1() is now ebook_prog_v1_drop() in components/ebook_progress. */

/* progress_load() is now ebook_prog_reload() in components/ebook_progress. */



/* slot_find() / slot_find_path() are now ebook_prog_lookup() /
 * ebook_prog_lookup_path() in components/ebook_progress. */

/* The on-chip NVS mirror (ensure / save / load) now lives in
 * components/ebook_progress (ebook_prog_save / ebook_prog_mirror_load). */

/* slot_alloc() and progress_create() are now internal to
 * components/ebook_progress. */

/* slot_flush() and progress_write() are now internal to
 * components/ebook_progress (reachable via ebook_prog_save). */

/* The context anchor: the EBOOK_CTX_LEN raw bytes at `off`. Used to verify a
 * restored offset and to re-locate it after the file was edited. */
static void ctx_read(size_t off, char *out)
{
    for (int i = 0; i < EBOOK_CTX_LEN; i++) {
        const int b = reader_byte(&s_reader, off + (size_t)i);
        out[i] = (b > 0) ? (char)b : '\0';
    }
}

/* Byte offset of the page that contains `target`: back up to a clean line
 * start, then lay out pages until the target is covered. Shared by the jump
 * and by the percentage fallback of the resume path. */
static size_t page_start_at(size_t target)
{
    size_t start = 0;
    if (target > 0) {
        size_t pos = target;
        while (pos > 0 && target - pos < EBOOK_CHUNK * 2) {
            if (reader_byte(&s_reader, pos - 1) == '\n') {
                start = pos;
                break;
            }
            pos--;
        }
        if (start == 0 && target > EBOOK_CHUNK) {
            start = target - EBOOK_CHUNK;
        }
    }
    size_t next = start;
    size_t prev = start;
    int guard = 0;
    while (next < target && guard++ < 1000) {
        prev = next;
        next = layout_page(&s_reader, next, NULL, 0);
        if (next == prev) {
            break;                       /* safety: no forward progress */
        }
    }
    return prev;
}

/* Arm a debounced save of the current position. Copies everything the save
 * task needs out of the UI-task-private state — including the context bytes,
 * which have to be read from the book right now. */
static void progress_arm(void)
{
    portENTER_CRITICAL(&s_mux);
    s_snap.fp = s_book_fp;
    strncpy(s_snap.path, s_book.path, sizeof(s_snap.path) - 1);
    s_snap.path[sizeof(s_snap.path) - 1] = '\0';
    const size_t off = s_cur_start;
    const size_t size = s_book.size;
    portEXIT_CRITICAL(&s_mux);

    s_snap.off = (uint32_t)off;
    s_snap.pct = size ? (uint8_t)((uint64_t)off * 100 / size) : 0;
    make_name(s_snap.name, sizeof(s_snap.name), s_snap.path);
    ctx_read(off, s_snap.ctx);           /* file I/O: outside the critical section */

    s_save_after = xTaskGetTickCount() + pdMS_TO_TICKS(EBOOK_SAVE_DELAY_MS);
    s_save_pending = true;
    s_flip_since_save++;
    if (s_flip_since_save >= EBOOK_SAVE_FORCE_PAGES) {
        /* Continuous flipping keeps resetting the debounce, so a long reading
         * session would never be persisted: force one through now. */
        s_save_after = 0;
        if (s_save_task != NULL) {
            xTaskNotifyGive(s_save_task);
        }
    }
}

/* Persist the armed snapshot. Safe to call from the save task and from the
 * UI task (ebook_progress_flush()). The actual file write and the NVS mirror
 * are handled by components/ebook_progress, which serializes against
 * concurrent loads/writers internally.
 * Returns false when the card rejected the write. */
static bool progress_save(void)
{
    /* Stale-snapshot guard: the book may have been switched since arm(); the
     * new book re-arms immediately, so a stale snapshot is simply dropped — not
     * an error. */
    if (s_snap.fp == 0 || s_snap.fp != s_book_fp) {
        return true;
    }
    const bool ok = ebook_prog_save(&s_snap);
    s_save_failed = !ok;
    if (ok) {
        s_flip_since_save = 0;
    } else {
        ESP_LOGW(TAG, "progress save failed for '%s'", s_snap.path);
    }
    return ok;
}

static void ebook_save_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_save_pending &&
            (int32_t)(s_save_after - xTaskGetTickCount()) <= 0) {
            const bool ok = progress_save();
            if (ok) {
                s_save_pending = false;
            } else {
                /* A transient SD write failure must not silently drop the
                 * latest position: keep it pending and retry after a short
                 * back-off. The next flip or a flush() will also re-attempt,
                 * but this guarantees a retry even if the user stops paging. */
                s_save_after = xTaskGetTickCount() +
                               pdMS_TO_TICKS(EBOOK_SAVE_RETRY_MS);
            }
        }
        /* ulTaskNotifyTake(pdTRUE) wakes immediately on a flush request and
         * consumes the notification, so no spurious wake-ups. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(EBOOK_SAVE_POLL_MS));
    }
}

/* Locate the saved position of the open book. Four levels, most to least
 * precise:
 *   EXACT    the offset is still a line start and its context bytes match
 *   DRIFT    the context bytes turned up within +/-8 KB of the offset: the
 *            book gained or lost text ahead of the reading position
 *   PERCENT  the context is gone (the file was re-encoded or reflowed): fall
 *            back to the saved byte percentage
 *   NONE     nothing usable — start at page 1
 * *start receives the page-start offset to lay out. */
static ebook_resume_t restore_position(const eb_slot_t *s, size_t size,
                                       size_t *start)
{
    if (s->off == 0 || s->off >= size) {
        return EBOOK_RESUME_NONE;
    }

    /* Level 1: the offset is still where the reader left it. */
    char ctx[EBOOK_CTX_LEN];
    ctx_read(s->off, ctx);
    const bool at_line_start =
        s->off == 0 || reader_byte(&s_reader, s->off - 1) == '\n';
    if (at_line_start && memcmp(ctx, s->ctx, EBOOK_CTX_LEN) == 0) {
        *start = s->off;
        return EBOOK_RESUME_EXACT;
    }

    /* Level 2: hunt for the context near the offset. The scan is sequential
     * through the reader window, so 16 KB costs a handful of chunk refills —
     * and it only ever runs on this path. */
    const size_t win = 8 * 1024;
    const size_t lo = (s->off > win) ? s->off - win : 0;
    size_t hi = s->off + win;
    if (hi > size) {
        hi = size;
    }
    size_t found = SIZE_MAX;
    for (size_t pos = lo; pos + EBOOK_CTX_LEN <= hi; pos++) {
        if (reader_byte(&s_reader, pos) != (uint8_t)s->ctx[0]) {
            continue;
        }
        bool match = true;
        for (int i = 1; i < EBOOK_CTX_LEN; i++) {
            if (reader_byte(&s_reader, pos + (size_t)i) !=
                (uint8_t)s->ctx[i]) {
                match = false;
                break;
            }
        }
        if (match) {
            found = pos;
            break;
        }
    }
    if (found != SIZE_MAX) {
        /* Back up to a line start so the page sequence stays stable. */
        const size_t guard = (found > EBOOK_CHUNK) ? found - EBOOK_CHUNK : 0;
        while (found > guard && reader_byte(&s_reader, found - 1) != '\n') {
            found--;
        }
        *start = found;
        return EBOOK_RESUME_DRIFT;
    }

    /* Level 3: the text is gone, the percentage is the last usable anchor. */
    if (s->pct > 0 && s->pct < 100) {
        *start = page_start_at((size_t)((uint64_t)size * s->pct / 100));
        return EBOOK_RESUME_PERCENT;
    }
    return EBOOK_RESUME_NONE;
}

/* Page number (1-based) of the canonical page starting at `off`, from the
 * count task's page-start table. Returns 0 when the table is not ready yet or
 * does not cover `off`. Pagination is deterministic, so for a session that
 * never jumped the answer is exact; after a jump it is within one page. */
static int page_from_table(size_t off)
{
    uint32_t n;
    portENTER_CRITICAL(&s_mux);
    n = s_table_valid ? s_table_count : 0;
    portEXIT_CRITICAL(&s_mux);
    if (n == 0 || s_start_table == NULL || s_start_table[0] > off) {
        return 0;
    }
    const uint32_t *t = s_start_table;
    uint32_t lo = 0, hi = n - 1, k = 0;
    while (lo <= hi) {                   /* k = last index with t[k] <= off */
        const uint32_t mid = (lo + hi) / 2;
        if (t[mid] <= (uint32_t)off) {
            k = mid;
            lo = mid + 1;
        } else if (mid == 0) {
            break;
        } else {
            hi = mid - 1;
        }
    }
    return (int)k + 1;
}

/* Replace the estimated page number with the exact one as soon as the count
 * task's table covers the current page. Called lazily from ebook_page() and
 * ebook_page_flip(), so it always runs on the UI task. */
static void page_calibrate(void)
{
    if (!s_page_calib) {
        return;
    }
    const int p = page_from_table(s_cur_start);
    if (p > 0) {
        s_page = p;
        s_page_calib = false;
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
    s_start_table = heap_caps_malloc(EBOOK_START_TABLE_CAP * sizeof(uint32_t),
                                     MALLOC_CAP_SPIRAM);
    if (s_start_table == NULL) {
        s_start_table = malloc(EBOOK_START_TABLE_CAP * sizeof(uint32_t));
    }
    if (s_start_table == NULL) {
        /* Not fatal: deep backward history falls back to the synchronous
         * whole-book relayout in rebuild_history(). */
        ESP_LOGW(TAG, "page-start table alloc failed (%u B): deep back-nav "
                      "falls back to slow relayout",
                 (unsigned)(EBOOK_START_TABLE_CAP * sizeof(uint32_t)));
    }
    s_reader.buf = s_chunk_a;
    s_reader.buf_cap = EBOOK_CHUNK;
    /* The progress persistence layer owns its own serialization; initialise it
     * here (after the SD card is mounted and NVS is up). */
    ebook_prog_init();
    xTaskCreate(ebook_scan_task, "eb_scan", 4 * 1024, NULL, 4, &s_scan_task);
    xTaskCreate(ebook_count_task, "eb_count", 4 * 1024, NULL, 4, &s_count_task);
    /* Same priority as the count task on purpose: that one lays out a whole
     * book in one long run, and at a lower priority it starved the debounced
     * save for as long as the walk took (tens of seconds on a large book), so
     * flipping pages and powering down lost everything since the last flush. */
    xTaskCreate(ebook_save_task, "eb_save", 4 * 1024, NULL, 4, &s_save_task);
}

void ebook_scan_start(void)
{
    ebook_scan_root(EBOOK_ROOT);
}

void ebook_scan_root(const char *dir)
{
    strncpy(s_load_root, dir ? dir : EBOOK_ROOT, sizeof(s_load_root) - 1);
    s_load_root[sizeof(s_load_root) - 1] = '\0';
    portENTER_CRITICAL(&s_mux);
    s_scan_pending = true;
    portEXIT_CRITICAL(&s_mux);
    if (s_scan_task != NULL) {
        xTaskNotifyGive(s_scan_task);
    }
}

bool ebook_scan_busy(void)
{
    bool b;
    portENTER_CRITICAL(&s_mux);
    b = s_scan_busy || s_scan_pending;
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
        /* Entries are full paths; the UI shows the file name only. */
        const char *p = s_scan_buf[s_scan_active][idx];
        const char *b = strrchr(p, '/');
        name = (b != NULL) ? b + 1 : p;
    }
    portEXIT_CRITICAL(&s_mux);
    return name;
}

const char *ebook_current_src_name(void)
{
    return s_src_name;
}

void ebook_close(void)
{
    if (s_reader.fp != NULL) {
        /* Closing a book is a low-frequency event: persist the position
         * synchronously so nothing is lost even if the debounced save never
         * fired (e.g. the user flipped once and immediately switched books).
         * arm() reads the context bytes, so it has to run before the fclose.
         * progress_save() delegates to components/ebook_progress, which
         * serializes against its own background save and concurrent loads.
         * Both no-op when the book has no fp. */
        progress_arm();
        progress_save();
        fclose(s_reader.fp);
        s_reader.fp = NULL;
    }
    s_save_pending = false;
    s_flip_since_save = 0;
    /* s_book / s_open_gen / s_is_open are copied by the count task under the
     * mutex; publish the reset under the same lock so it never observes a
     * torn struct. Everything below is UI-task-private. */
    portENTER_CRITICAL(&s_mux);
    memset(&s_book, 0, sizeof(s_book));
    s_book_fp = 0;
    s_is_open = false;
    s_open_gen++;
    s_page = 0;
    s_page_count = 0;
    s_table_valid = false;
    s_table_count = 0;
    portEXIT_CRITICAL(&s_mux);
    s_cur_start = s_next_start = 0;
    s_session_start = 0;
    s_hist_count = s_hist_head = 0;
    s_page_buf[0] = '\0';
    s_page_calib = false;
    s_resume_kind = EBOOK_RESUME_NONE;
    s_resume_pct = 0;
}

bool ebook_open(int idx)
{
    char path[EBOOK_PATH_MAX];
    portENTER_CRITICAL(&s_mux);
    if (idx < 0 || idx >= s_scan_count) {
        portEXIT_CRITICAL(&s_mux);
        return false;
    }
    strncpy(path, s_scan_buf[s_scan_active][idx], EBOOK_PATH_MAX - 1);
    path[EBOOK_PATH_MAX - 1] = '\0';
    portEXIT_CRITICAL(&s_mux);

    ebook_close();

    /* Build the source description in a local first, then publish it under
     * the mutex in one go: the count task copies s_book atomically, so a
     * torn struct (e.g. a garbage src.size) could otherwise send its
     * page-count loop spinning forever at EOF. */
    book_src_t src;
    memset(&src, 0, sizeof(src));

    snprintf(src.path, sizeof(src.path), "%s", path);
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
    s_reader.base = 0;
    s_reader.len = 0;

    /* Identify the book by its content, not by its path: a rename, a move to
     * another folder or a re-copied card all keep the same fingerprint, so
     * the reading position follows the book. Two seeks and 2 KB of reads.
     *
     * If the sample cannot be read at all, fall back to a path-derived id
     * rather than to 0: the position is then still remembered exactly as v1
     * did, it just no longer survives a rename. 0 is reserved for "empty
     * slot" and would silently disable saving for the whole book. */
    uint64_t book_fp = ebook_prog_fingerprint(src.path, src.size);
    if (book_fp == 0) {
        book_fp = ebook_prog_fnv1a64((const uint8_t *)src.path, strlen(src.path),
                                     0xcbf29ce484222325ULL);
        if (book_fp == 0) {
            book_fp = 1;                 /* never 0: that marks an empty slot */
        }
        ESP_LOGW(TAG, "fingerprint sample failed, using path id for '%s'",
                 src.path);
    }

    portENTER_CRITICAL(&s_mux);
    s_book = src;
    s_book_fp = book_fp;
    s_is_open = true;
    s_open_gen++;                        /* start the count fresh for this book */
    /* The page-start table belongs to the previous book: invalidate it until
     * the new count walk publishes its own (its entries are written outside
     * the mutex, so readers must never see them while stale). */
    s_table_valid = false;
    s_table_count = 0;
    portEXIT_CRITICAL(&s_mux);

    /* Restore the saved position. The v2 slot is keyed by a content fingerprint
     * and carries context bytes, so restore_position() can tell an exact match
     * from a drifted one and degrades gracefully (DRIFT / PERCENT / NONE) when
     * the file changed. A v2 miss falls back to a one-time migration of the
     * legacy v1 record (matched by path hash + size): it seeds a v2 slot and is
     * then validated by restore_position(), so a book whose contents changed
     * (only its size coincidentally matches) still degrades instead of resuming
     * to a wrong offset. Leftover v1 files are removed by the 清除进度 option. */
    size_t start = 0;
    s_resume_pct = 0;
    s_resume_kind = EBOOK_RESUME_NONE;
    if (src.size > 0 && book_fp != 0) {
        ebook_prog_reload();
        const uint32_t src_path_h = ebook_prog_fnv1a32(src.path);
        eb_slot_t slot;
        bool have = ebook_prog_lookup(book_fp, &slot);
        if (!have) {
            have = ebook_prog_lookup_path(src_path_h, &slot);
        }
        if (have) {
            s_resume_kind = restore_position(&slot, src.size, &start);
        }
        else {
            /* v2 miss: recover the book's legacy v1 record (path hash + size).
             * v1 kept no context, so instead of resuming straight from its
             * offset we seed a v2 slot and let restore_position() validate it;
             * if the file really changed the context check fails and it degrades
             * to PERCENT / NONE rather than lying about an exact match. The v1
             * entry is dropped so it can't be re-matched on a later open. */
            uint32_t v1_off = 0;
            if (ebook_prog_v1_lookup(src.path, src.size, &v1_off) &&
                v1_off > 0 && v1_off < src.size) {
                char ctx[EBOOK_CTX_LEN];
                ctx_read(v1_off, ctx);
                eb_snap_t m;
                memset(&m, 0, sizeof(m));
                m.fp = book_fp;
                m.off = (uint32_t)v1_off;
                m.pct = (uint8_t)((uint64_t)v1_off * 100 / src.size);
                memcpy(m.path, src.path, sizeof(m.path) - 1);
                memcpy(m.ctx, ctx, EBOOK_CTX_LEN);
                ebook_prog_save(&m);
                ebook_prog_v1_drop(src.path);
                if (ebook_prog_lookup(book_fp, &slot)) {
                    s_resume_kind = restore_position(&slot, src.size, &start);
                }
            }
        }
        /* Last-resort fallback: the card is missing / the slot was lost, but a
         * previous save mirrored this book (by fingerprint or by name) to NVS.
         * restore_position() still validates it, so a changed file degrades
         * instead of jumping to a wrong page. */
        if (s_resume_kind == EBOOK_RESUME_NONE) {
            eb_slot_t nv;
            if (ebook_prog_mirror_load(&nv) &&
                (nv.fp == book_fp || nv.path_h == src_path_h)) {
                s_resume_kind = restore_position(&nv, src.size, &start);
                if (s_resume_kind != EBOOK_RESUME_NONE) {
                    ESP_LOGI(TAG, "progress: resumed from NVS mirror '%s' kind=%d",
                             nv.name, (int)s_resume_kind);
                }
            }
        }
    }
    if (s_resume_kind == EBOOK_RESUME_NONE) {
        start = 0;
    }
    s_resume_pct = (uint32_t)((uint64_t)start * 100 / (src.size ? src.size : 1));

    /* The restored position is the origin of the current page sequence:
     * backward history is rebuilt from here (see s_session_start). */
    s_session_start = start;
    s_cur_start = start;
    /* The page number is no longer persisted: after a jump it was only an
     * estimate, and storing it made the "X/N" readout drift. Take it from the
     * count task's page-start table instead, or flag it for calibration as
     * soon as that table lands (page_calibrate()). */
    s_page = page_from_table(start);
    s_page_calib = (s_page == 0);
    if (s_page == 0) {
        s_page = 1;
    }
    s_next_start = layout_page(&s_reader, start, s_page_buf,
                               sizeof(s_page_buf));
    if (s_count_task != NULL) {
        xTaskNotifyGive(s_count_task);
    }
    ESP_LOGI(TAG, "open '%s' size=%u fp=%08X%08X resume=%u%% kind=%d page=%d",
             path, (unsigned)src.size,
             (unsigned)(uint32_t)(book_fp >> 32), (unsigned)(uint32_t)book_fp,
             (unsigned)s_resume_pct, (int)s_resume_kind, s_page);
    return true;
}

int ebook_page(void)
{
    page_calibrate();                /* exact number once the count lands */
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
    /* The offset, not the page number: s_page is only an estimate until the
     * count task's table lands, and after a resume it may still be 1 while
     * the reader sits in the middle of the book. */
    return s_cur_start == 0;
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
    page_calibrate();                /* keep the +1/-1 off an estimate */
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
        if (s_cur_start == 0) {
            return;                      /* first page of the book */
        }
        size_t prev;
        if (!hist_pop(&prev)) {
            rebuild_history();           /* also restores the pre-resume pages */
            if (!hist_pop(&prev)) {
                return;
            }
        }
        s_cur_start = prev;
        s_next_start = layout_page(&s_reader, s_cur_start,
                                   s_page_buf, sizeof(s_page_buf));
        s_page--;
    }
    if (s_book.path[0] != '\0') {
        progress_arm();                  /* debounced save of the new position */
    }
}

uint32_t ebook_resume_percent(void)
{
    return s_resume_pct;
}

ebook_resume_t ebook_resume_kind(void)
{
    return s_resume_kind;
}

bool ebook_save_failed(void)
{
    return s_save_failed;
}

bool ebook_progress_flush(void)
{
    if (!s_is_open) {
        return true;
    }
    progress_arm();
    /* Save here rather than waking the save task: the caller (leaving the
     * reader) needs to know whether the position reached the card. One slot
     * write, a few ms. */
    s_save_pending = false;
    s_save_after = 0;
    return progress_save();
}

/* Erase every saved reading position from the card, including the legacy v1
 * path-hash table. v1 is the usual source of "resumes to the wrong book": it
 * keys only on path hash + file size, so two books that happen to share a size
 * can swap positions. Wiping both files removes all residual progress. The
 * persistence layer (components/ebook_progress) owns the card file, the cache
 * and the NVS mirror, so it handles the wipe atomically. Safe to call at any
 * time (serialized with the save path); the caller should be on the UI task. */
void ebook_progress_clear_all(void)
{
    ebook_prog_clear_all();
}

bool ebook_jump_percent(int pct)
{
    if (!s_is_open || s_book.size == 0) {
        return false;
    }
    if (pct < 0) {
        pct = 0;
    }
    if (pct > 100) {
        pct = 100;
    }
    const size_t target =
        (size_t)((uint64_t)s_book.size * (uint32_t)pct / 100);
    const size_t start = page_start_at(target);

    /* The jump target becomes the origin of the current page sequence: the
     * pages after a jump are laid out from here and are not canonical (see
     * s_session_start / rebuild_history). */
    s_session_start = start;
    s_cur_start = start;
    s_next_start = layout_page(&s_reader, start, s_page_buf,
                               sizeof(s_page_buf));
    s_hist_count = 0;
    s_hist_head = 0;
    /* The page number comes from the count task's table when it covers this
     * offset; otherwise estimate it from the percentage and flag it for
     * calibration. The byte progress bar stays the authoritative indicator. */
    s_page = page_from_table(start);
    s_page_calib = (s_page == 0);
    if (s_page == 0) {
        s_page = (s_page_count > 0)
                     ? (int)((uint64_t)pct * s_page_count / 100)
                     : 1;
        if (s_page < 1) {
            s_page = 1;
        }
    }
    s_resume_kind = EBOOK_RESUME_NONE;   /* a jump is not a restore */
    s_resume_pct = 0;
    if (s_book.path[0] != '\0') {
        progress_arm();                  /* remember the jumped-to position */
    }
    ESP_LOGI(TAG, "jump to %d%% -> offset %u", pct, (unsigned)start);
    return true;
}

const char *ebook_page_text(void)
{
    return s_page_buf;
}
