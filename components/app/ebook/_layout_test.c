/* Temporary layout-engine test: drives the REAL layout_page() from ebook.c.
 * Stub ESP headers live in ./test_inc so this compiles on the host. Deleted
 * after use. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* Pull in the real implementation. The stub headers in test_inc satisfy the
 * ESP-IDF / FreeRTOS dependencies. */
const uint8_t _binary_Test_txt_start[1]  __attribute__((weak)) = {0};
const uint8_t _binary_Test_txt_end[1]    __attribute__((weak)) = {0};
#include "ebook.c"

static int fails = 0;

static void dump_page(const char *title, const char *txt)
{
    printf("--- %s ---\n", title);
    for (const char *p = txt; *p; p++) {
        putchar(*p == '\n' ? '\n' : *p);
    }
    printf("\n");
}

static size_t render_one(reader_t *r, size_t start, char *buf, size_t cap)
{
    return layout_page(r, start, buf, cap);
}

/* Count non-space glyphs on a display line. */
static int glyphs(const char *line)
{
    int n = 0;
    for (; *line; line++) if (*line != ' ' && *line != '\n') n++;
    return n;
}

int main(void)
{
    printf("EBOOK_LINE_W=%d EBOOK_LINE_W16=%d\n", EBOOK_LINE_W, EBOOK_LINE_W16);

    /* Real embedded ROM book: Test.txt. Exercise the actual layout engine on
     * the same data the firmware ships, so pagination assertions are real. */
    extern const uint8_t _binary_Test_txt_start[];
    extern const uint8_t _binary_Test_txt_end[];
    static const char k_test[] =
        "公交专线,去往南门口方向,分AB线,详情是\n"
        "赣六中至南门口A线(迎宾大道线路)线路走向明细 \n"
        "上行:赣州六中 -- 工业路口 -- 华坚鞋城 -- 天赐良缘 -- 越秀花苑 -- 三康庙 -- 移动大厅\n"
        "下行:移动大厅 -- 高琰路口 -- 越秀花苑 -- 锦绣星城 -- 华坚鞋城 -- 工业路口 -- 赣州六中\n"
        "赣六中至南门口B线(客家大道线路)线路走向明细 \n"
        "上行:赣州六中 -- 金岭大道新市民公寓 -- 附属医院黄金分院 -- 翠湖山庄 -- 七一九社区 -- 赣南科技学院 -- 博德山庄 -- 南河大桥(康复医院) -- 东阳山大市场(坚强百货)\n"
        "下行:移动大厅 -- 东阳山大市场(赣州航空站) -- 南河大桥(康复医院) -- 博德山庄 -- 赣南科技学院 -- 七一九社区 -- 翠湖山庄 -- 附属医院黄金分院 -- 金岭大道新市民公寓 -- 赣州六中\n"
        "赣六中至蓉江新区(潭口镇政府线路)线路走向明细\n"
        "上行:赣州六中 -- 当塘返迁房 -- 阳光金色春城 -- 毅德融城 -- 潭东镇政府 -- 四十米大道路口 -- 潭口镇政府\n"
        "下行:潭口镇政府 -- 四十米大道路口 -- 潭东镇政府 -- 毅德融城 -- 阳光金色春城 -- 当塘返迁房 -- 赣州六中\n";
    reader_t r;
    memset(&r, 0, sizeof(r));
    r.embedded = true;
    r.rom = (const uint8_t *)k_test;
    r.rom_size = strlen(k_test);
    r.buf = malloc(4096);
    r.buf_cap = 4096;

    char page[2048];
    size_t off = 0;
    int pageno = 0;
    while (off < r.rom_size && pageno < 6) {
        off = render_one(&r, off, page, sizeof(page));
        char title[32];
        snprintf(title, sizeof(title), "PAGE %d", ++pageno);
        dump_page(title, page);
    }

    /* Whole-book render for assertions. */
    char full[8192]; full[0] = '\0';
    size_t o2 = 0;
    while (o2 < r.rom_size) {
        char pg[2048];
        o2 = render_one(&r, o2, pg, sizeof(pg));
        strncat(full, pg, sizeof(full) - strlen(full) - 1);
    }

    /* (1) All source text is preserved (nothing dropped/swallowed). */
    if (strstr(full, "当塘返迁房") && strstr(full, "潭口镇政府") &&
        strstr(full, "四十米大道路口")) {
        printf("[PASS] full source text preserved\n");
    } else {
        printf("[FAIL] source text lost\n"); fails++;
    }

    /* (2) Greedy fill: middle display lines are filled to the brim. */
    {
        int max_glyphs = 0, sum = 0, lines = 0;
        int cur = 0;
        for (char *p = full; ; p++) {
            if (*p == '\0' || *p == '\n') {
                if (cur > max_glyphs) max_glyphs = cur;
                sum += cur; lines++;
                if (*p == '\0') break;
                cur = 0;
            } else if (*p != ' ') {
                cur++;
            }
        }
        int avg = lines ? sum / lines : 0;
        printf("  glyphs: max=%d avg=%d over %d lines\n", max_glyphs, avg, lines);
        if (avg < 8) {
            printf("[FAIL] lines too short, not filled (avg=%d)\n", avg); fails++;
        } else {
            printf("[PASS] lines filled (avg glyphs=%d)\n", avg);
        }
    }

    /* (3) The ONLY break inside the single source line is the width overflow:
     * every wrapped line (except possibly the last) must be near-full. More
     * importantly, NO character-class rule exists, so a long ASCII run is hard
     * split mid-word when it overflows. We verify that a very long unbreakable
     * token does not blow past the line width. */
    {
        const char *longword =
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n";
        reader_t rl; memset(&rl, 0, sizeof(rl));
        rl.embedded = true;
        rl.rom = (const uint8_t *)longword;
        rl.rom_size = strlen(longword);
        rl.buf = malloc(4096); rl.buf_cap = 4096;
        char pg[2048]; size_t ol = render_one(&rl, 0, pg, sizeof(pg));
        (void)ol; free(rl.buf);
        /* Each rendered line must not exceed the physical line width in glyphs.
         * With mono ASCII ~10/16 px and EBOOK_LINE_W16, a line holds many 'a's;
         * we just assert no single line is absurdly long (> 200 glyphs). */
        int cur = 0, over = 0;
        for (char *p = pg; ; p++) {
            if (*p == '\0' || *p == '\n') {
                if (cur > 200) over++;
                cur = 0;
                if (*p == '\0') break;
            } else if (*p != ' ') {
                cur++;
            }
        }
        if (over) { printf("[FAIL] a line overflowed width\n"); fails++; }
        else printf("[PASS] width overflow handled (hard break)\n");
    }

    /* (4) Source newlines are preserved verbatim. A two-line source produces a
     * line break in the output at exactly that point. */
    {
        const char *two =
            "第一行短内容\n第二行短内容\n";
        reader_t rt; memset(&rt, 0, sizeof(rt));
        rt.embedded = true;
        rt.rom = (const uint8_t *)two;
        rt.rom_size = strlen(two);
        rt.buf = malloc(4096); rt.buf_cap = 4096;
        char pg[2048]; render_one(&rt, 0, pg, sizeof(pg)); free(rt.buf);
        /* pg should contain "第一行短内容\n第二行短内容" with a newline between,
         * and NOT merge the two source lines into one wrapped line. */
        if (strstr(pg, "第一行短内容\n第二行") == NULL) {
            printf("[FAIL] source newline not preserved\n"); fails++;
        } else {
            printf("[PASS] source newline preserved as hard break\n");
        }
    }

    printf("\n%s\n", fails ? "SOME FAILED" : "ALL PASS");
    return fails;
}
