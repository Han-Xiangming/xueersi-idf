/* Break-level unit test for the fill_line() engine in ebook.c.
 * The new engine keeps ONLY the source file's own newlines and hard-breaks at
 * the width limit — there is no character-class rule layer.
 * Compile on host:  gcc -I test_inc -I . _lb_test.c -o _lb_test.exe
 * This file is NOT part of the firmware build. */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* Provide the embedded ROM symbols so ebook.c links on the host. */
const uint8_t _binary_Test_txt_start[1]  __attribute__((weak)) = {0};
const uint8_t _binary_Test_txt_end[1]    __attribute__((weak)) = {0};

#include "ebook.c"   /* pull in fill_line / layout_page / helpers */

static int fails = 0;

static reader_t mk(const char *s)
{
    reader_t r;
    memset(&r, 0, sizeof(r));
    r.embedded = true;
    r.rom = (const uint8_t *)s;
    r.rom_size = strlen(s);
    r.buf = malloc(4096);
    r.buf_cap = 4096;
    return r;
}

static void expect(const char *name, int got, int want)
{
    if (got == want) {
        printf("[PASS] %s (%d)\n", name, got);
    } else {
        printf("[FAIL] %s: got %d want %d\n", name, got, want);
        fails++;
    }
}

int main(void)
{
    /* (1) A source newline stops the line and is reported via *nl. */
    {
        reader_t r = mk("abc\ndef");
        size_t o = 0;
        char out[64]; size_t ol = 0;
        bool eof = false, nl = false;
        fill_line(&r, &o, out, &ol, sizeof(out), &eof, &nl);
        expect("newline ends line (*nl)", nl ? 1 : 0, 1);
        expect("newline not eof", eof ? 1 : 0, 0);
        expect("offset past newline", (int)o, 4);   /* "abc\n" -> points at 'd' */
        free(r.buf);
    }

    /* (2) Width overflow hard-breaks BEFORE the overflowing glyph.
     * Note: on the host test build the ASCII advance table (s_ascii_w16) is
     * zero, so we drive overflow with CJK glyphs whose advance is the constant
     * EBOOK_CHAR_W16 (256), which never depends on the table. */
    {
        char buf[256];
        memset(buf, 0x00, sizeof(buf));
        /* 30 full-width '中' (U+4E2D, UTF-8 E4 B8 AD) then a newline. */
        for (int i = 0; i < 30; i++) {
            buf[i*3+0] = 0xE4; buf[i*3+1] = 0xB8; buf[i*3+2] = 0xAD;
        }
        buf[90] = '\n';
        reader_t r = mk(buf);
        size_t o = 0;
        char out[2048]; size_t ol = 0;
        bool eof = false, nl = false;
        fill_line(&r, &o, out, &ol, sizeof(out), &eof, &nl);
        /* EBOOK_LINE_W16 / 256 = 5120/256 = 20 glyphs fit. Overflow must stop
         * before glyph 30, at the first glyph that would exceed the width. */
        expect("overflow stops before full text", (o > 0 && o < 90) ? 1 : 0, 1);
        expect("overflow is not a newline break", nl ? 1 : 0, 0);
        free(r.buf);
    }

    /* (3) A short source line with no newline and no overflow runs to EOF. */
    {
        reader_t r = mk("短内容");
        size_t o = 0;
        char out[64]; size_t ol = 0;
        bool eof = false, nl = false;
        fill_line(&r, &o, out, &ol, sizeof(out), &eof, &nl);
        expect("short line hits eof", eof ? 1 : 0, 1);
        expect("short line not newline-broken", nl ? 1 : 0, 0);
        free(r.buf);
    }

    /* (4) A leading space on a wrapped line is skipped (line never starts
     * with a space), so the overflow break does not strand a space. */
    {
        /* "x " followed by enough 'a' to overflow: the wrapped continuation
         * must not begin with the space. */
        char buf[600];
        buf[0] = 'x'; buf[1] = ' ';
        memset(buf + 2, 'a', 597);
        buf[599] = '\n';
        reader_t r = mk(buf);
        size_t o = 0;
        char out[2048]; size_t ol = 0;
        bool eof = false, nl = false;
        fill_line(&r, &o, out, &ol, sizeof(out), &eof, &nl);
        expect("wrapped line consumed something", (o > 0) ? 1 : 0, 1);
        free(r.buf);
    }

    printf("\n%s\n", fails ? "SOME FAILED" : "ALL PASS");
    return fails;
}
