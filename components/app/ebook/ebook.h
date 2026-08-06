/*
 * Application layer: TXT ebook reader.
 *
 * Streams UTF-8 plain-text books from the SD card (/sdcard, all .txt files,
 * plus a built-in ROM test book) and lays them out into fixed 5-line pages
 * with the 16px CJK font. Pagination is
 * deterministic (independent of LVGL), so forward/backward flips and the
 * background page-count task always agree. See docs/ebook.md.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Create the scan/count tasks and the PSRAM chunk buffers. Call once. */
void ebook_init(void);

/* Background scan of /sdcard for .txt books (idempotent while one runs). */
void ebook_scan_start(void);
bool ebook_scan_busy(void);
uint32_t ebook_scan_version(void);     /* bumps when the list is replaced */
int  ebook_scan_count(void);
const char *ebook_scan_name(int idx);  /* NUL-terminated, no path */

bool ebook_open(int idx);              /* open book idx, position at page 1 */
void ebook_close(void);

int  ebook_page(void);                 /* current page, 1-based */
int  ebook_page_count(void);           /* total pages; 0 until counted */
uint32_t ebook_count_version(void);    /* bumps when the count lands/changes */
int  ebook_percent(void);              /* byte progress 0..100 */

bool ebook_at_start(void);
bool ebook_at_end(void);
void ebook_page_flip(int dir);         /* +1 next page / -1 previous page */
const char *ebook_page_text(void);     /* current page, lines joined by '\n' */
