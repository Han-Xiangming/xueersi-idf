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
#include <sys/stat.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

/* Reading-progress persistence on the SD card (single file next to the
 * books, so it moves with the card and survives firmware updates). A
 * debounced save runs EBOOK_SAVE_DELAY_MS after the last page flip;
 * ebook_close() / flush save immediately as a fallback. */
#define EBOOK_PROG_FILE     EBOOK_ROOT "/.progress"
#define EBOOK_PROG_TMP      EBOOK_ROOT "/.progress.tmp"
#define EBOOK_PROG_MAGIC    0x47504245   /* "EBPG" */
#define EBOOK_PROG_MAX      512          /* remembered books (MRU, oldest dropped) */
#define EBOOK_SAVE_DELAY_MS 3000
#define EBOOK_SAVE_POLL_MS  500

/* Saved per-book position entry. `hash` is the 48-bit FNV-1a of the book
 * path; `size` guards against a changed file: the position is only restored
 * when it matches the current file size. */
typedef struct {
    uint64_t hash;
    uint32_t offset;
    uint32_t page;
    uint32_t size;
} eb_progress_t;

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

static reader_t s_reader;
static book_src_t s_book;

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
 * writes it out when the open book still matches s_snap_path (a book switch
 * re-arms immediately, so the snapshot always belongs to the current book).
 * The on-card file is mirrored into s_prog[] (MRU order) on open; saves
 * update the cache and rewrite the file atomically (tmp + rename). */
static char s_snap_path[EBOOK_PATH_MAX];
static uint32_t s_snap_off;
static uint32_t s_snap_page;
static uint32_t s_snap_size;
static bool s_save_pending;
static uint32_t s_save_after;            /* tick deadline for the debounce */
static uint32_t s_resume_pct;            /* restored position %, 0 = page 1 */
static TaskHandle_t s_save_task;

static eb_progress_t s_prog[EBOOK_PROG_MAX] EXT_RAM_BSS_ATTR;
static int s_prog_count;                 /* entries currently in s_prog[] */
static bool s_prog_loaded;               /* false until a load attempt ran */

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

/* 48-bit FNV-1a of the book path, used as the entry key. 48 bits make a
 * collision between different paths vanishingly unlikely. */
static uint64_t fnv1a48(const char *s)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 0x100000001b3ULL;
    }
    return h & 0xFFFFFFFFFFFFULL;
}

/* Load the progress file into s_prog[] (MRU order). A missing or corrupt
 * file (bad magic / truncated) yields an empty list; the next save rebuilds
 * it. Called on every book open, so a card swap or external edit takes
 * effect immediately. */
static void progress_load(void)
{
    s_prog_count = 0;
    s_prog_loaded = true;
    FILE *fp = fopen(EBOOK_PROG_FILE, "rb");
    if (fp == NULL) {
        return;
    }
    uint32_t magic = 0, count = 0;
    if (fread(&magic, 4, 1, fp) == 1 && fread(&count, 4, 1, fp) == 1 &&
        magic == EBOOK_PROG_MAGIC && count > 0 && count <= EBOOK_PROG_MAX) {
        s_prog_count = (int)fread(s_prog, sizeof(eb_progress_t), count, fp);
    }
    fclose(fp);
}

/* Atomically persist s_prog[] (tmp file + rename, so a power cut mid-write
 * leaves the old file intact). */
static void progress_store(void)
{
    FILE *fp = fopen(EBOOK_PROG_TMP, "wb");
    if (fp == NULL) {
        return;
    }
    const uint32_t magic = EBOOK_PROG_MAGIC;
    const uint32_t count = (uint32_t)s_prog_count;
    fwrite(&magic, 4, 1, fp);
    fwrite(&count, 4, 1, fp);
    fwrite(s_prog, sizeof(eb_progress_t), (size_t)count, fp);
    fclose(fp);
    rename(EBOOK_PROG_TMP, EBOOK_PROG_FILE);
}

static void progress_touch(int idx)
{
    eb_progress_t t = s_prog[idx];
    memmove(&s_prog[1], &s_prog[0], (size_t)idx * sizeof(eb_progress_t));
    s_prog[0] = t;
}

static void progress_write(const char *path, uint32_t off, uint32_t page,
                           uint32_t size)
{
    if (path[0] == '\0') {
        return;                          /* no path: nothing to save */
    }
    if (!s_prog_loaded) {
        progress_load();
    }
    const uint64_t h = fnv1a48(path);
    int idx = -1;
    for (int i = 0; i < s_prog_count; i++) {
        if (s_prog[i].hash == h) {
            idx = i;
            break;
        }
    }
    if (idx >= 0) {
        s_prog[idx].offset = off;
        s_prog[idx].page = page;
        s_prog[idx].size = size;
        progress_touch(idx);             /* most-recently-used first */
    }
    else if (s_prog_count < EBOOK_PROG_MAX) {
        memmove(&s_prog[1], &s_prog[0],
                (size_t)s_prog_count * sizeof(eb_progress_t));
        s_prog[0] = (eb_progress_t){ h, off, page, size };
        s_prog_count++;
    }
    else {
        /* Cache full: replace the least-recently-used entry (last). */
        s_prog[EBOOK_PROG_MAX - 1] = (eb_progress_t){ h, off, page, size };
        progress_touch(EBOOK_PROG_MAX - 1);
    }
    progress_store();
}

/* Look up a book's saved position in the loaded cache. Returns true and
 * fills *p when present. */
static bool progress_find(const char *path, eb_progress_t *p)
{
    const uint64_t h = fnv1a48(path);
    for (int i = 0; i < s_prog_count; i++) {
        if (s_prog[i].hash == h) {
            *p = s_prog[i];
            return true;
        }
    }
    return false;
}

static void progress_arm(void)
{
    portENTER_CRITICAL(&s_mux);
    strncpy(s_snap_path, s_book.path, sizeof(s_snap_path) - 1);
    s_snap_path[sizeof(s_snap_path) - 1] = '\0';
    s_snap_off = (uint32_t)s_cur_start;
    s_snap_page = (uint32_t)s_page;
    s_snap_size = s_book.size;
    portEXIT_CRITICAL(&s_mux);
    s_save_after = xTaskGetTickCount() + pdMS_TO_TICKS(EBOOK_SAVE_DELAY_MS);
    s_save_pending = true;
}

static void progress_save(void)
{
    s_save_pending = false;
    char path[EBOOK_PATH_MAX];
    uint32_t off, page, size;
    portENTER_CRITICAL(&s_mux);
    if (strcmp(s_snap_path, s_book.path) != 0) {
        portEXIT_CRITICAL(&s_mux);
        return;                          /* book switched: stale snapshot */
    }
    strncpy(path, s_snap_path, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    off = s_snap_off;
    page = s_snap_page;
    size = s_snap_size;
    portEXIT_CRITICAL(&s_mux);
    progress_write(path, off, page, size);
}

static void ebook_save_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_save_pending &&
            (int32_t)(s_save_after - xTaskGetTickCount()) <= 0) {
            progress_save();
        }
        /* ulTaskNotifyTake(pdTRUE) wakes immediately on a flush request and
         * consumes the notification, so no spurious wake-ups. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(EBOOK_SAVE_POLL_MS));
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
    xTaskCreate(ebook_scan_task, "eb_scan", 4 * 1024, NULL, 4, &s_scan_task);
    xTaskCreate(ebook_count_task, "eb_count", 4 * 1024, NULL, 4, &s_count_task);
    xTaskCreate(ebook_save_task, "eb_save", 4 * 1024, NULL, 2, &s_save_task);
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
         * fired (e.g. the user flipped once and immediately switched books). */
        uint32_t off = (uint32_t)s_cur_start;
        uint32_t page = (uint32_t)s_page;
        uint32_t size = s_book.size;
        char path[EBOOK_PATH_MAX];
        portENTER_CRITICAL(&s_mux);
        strncpy(path, s_book.path, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
        portEXIT_CRITICAL(&s_mux);
        progress_write(path, off, page, size);

        fclose(s_reader.fp);
        s_reader.fp = NULL;
    }
    s_save_pending = false;
    /* s_book / s_open_gen / s_is_open are copied by the count task under the
     * mutex; publish the reset under the same lock so it never observes a
     * torn struct. Everything below is UI-task-private. */
    portENTER_CRITICAL(&s_mux);
    memset(&s_book, 0, sizeof(s_book));
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

    portENTER_CRITICAL(&s_mux);
    s_book = src;
    s_is_open = true;
    s_open_gen++;                        /* start the count fresh for this book */
    /* The page-start table belongs to the previous book: invalidate it until
     * the new count walk publishes its own (its entries are written outside
     * the mutex, so readers must never see them while stale). */
    s_table_valid = false;
    s_table_count = 0;
    portEXIT_CRITICAL(&s_mux);

    /* Restore the saved reading position, if any: reload the on-card
     * progress file (picks up card swaps / external edits), then require the
     * book to be unchanged (same size) and the offset to point inside it and
     * past page 1. The stored page number is used as-is, so the "X/N"
     * readout matches where the reader left off; the byte progress bar is
     * authoritative. */
    size_t start = 0;
    int page = 1;
    s_resume_pct = 0;
    if (src.size > 0) {
        progress_load();
        eb_progress_t p;
        if (progress_find(src.path, &p) && p.size == src.size &&
            p.offset > 0 && p.offset < src.size && p.page > 0) {
            start = p.offset;
            page = (int)p.page;
            s_resume_pct = (uint32_t)((uint64_t)p.offset * 100 / src.size);
        }
    }
    /* The restored position is the origin of the current page sequence:
     * backward history is rebuilt from here (see s_session_start). */
    s_session_start = start;
    s_cur_start = start;
    s_page = page;
    s_next_start = layout_page(&s_reader, start, s_page_buf,
                               sizeof(s_page_buf));
    if (s_count_task != NULL) {
        xTaskNotifyGive(s_count_task);
    }
    ESP_LOGI(TAG, "open '%s' size=%u resume=%u%%", path,
             (unsigned)src.size, (unsigned)s_resume_pct);
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
    if (s_book.path[0] != '\0') {
        progress_arm();                  /* debounced save of the new position */
    }
}

uint32_t ebook_resume_percent(void)
{
    return s_resume_pct;
}

void ebook_progress_flush(void)
{
    if (!s_is_open) {
        return;
    }
    progress_arm();
    s_save_after = 0;                    /* expire the debounce: save on wake */
    if (s_save_task != NULL) {
        xTaskNotifyGive(s_save_task);    /* save immediately, don't wait */
    }
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

    /* Back up to a line start near the target so the first page begins on a
     * clean line. A long unbroken paragraph (no '\n' within the search
     * window) falls back to starting about one chunk (~1KB) before the
     * target, which keeps the backward scan bounded. */
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

    /* Layout forward from `start` to the page that contains `target` (usually
     * one or two pages; the backward line-search made the distance small). */
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
    start = prev;

    /* The jump target becomes the origin of the current page sequence: the
     * pages after a jump are laid out from here and are not canonical (see
     * s_session_start / rebuild_history). */
    s_session_start = start;
    s_cur_start = start;
    s_next_start = layout_page(&s_reader, start, s_page_buf,
                               sizeof(s_page_buf));
    s_hist_count = 0;
    s_hist_head = 0;
    /* Estimate the page number for the "X/N" readout — the byte progress bar
     * stays the authoritative indicator. Falls back to 1 while the total is
     * still being counted. */
    if (s_page_count > 0) {
        int est = (int)((uint64_t)pct * s_page_count / 100);
        s_page = est < 1 ? 1 : est;
    }
    else {
        s_page = 1;
    }
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
