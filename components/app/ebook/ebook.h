/*
 * Application layer: TXT ebook reader.
 *
 * Streams UTF-8 plain-text books from the SD card (the /sdcard/eBook tree,
 * scanned recursively) and lays them out into fixed 8-line pages with the 16px CJK font. Pagination is
 * deterministic (independent of LVGL), so forward/backward flips and the
 * background page-count task always agree. Reading position is remembered
 * per book on the SD card (identified by a content fingerprint, not by its
 * path) and restored on open. See docs/ebook.md.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Scan root: books live in /sdcard/eBook, optionally in sub-folders (each
 * top-level folder is one selectable "source", like the music player). */
#define EBOOK_ROOT "/sdcard/eBook"

/* Create the scan/count/save tasks and the PSRAM chunk buffers. Call once. */
void ebook_init(void);

/* Background scan of the whole EBOOK_ROOT tree for .txt books. */
void ebook_scan_start(void);
/* Background scan of one source directory (a top-level folder under
 * EBOOK_ROOT, or EBOOK_ROOT itself for the whole tree). Requests merge:
 * a scan landing while another runs is re-done. */
void ebook_scan_root(const char *dir);
bool ebook_scan_busy(void);
uint32_t ebook_scan_version(void);     /* bumps when the list is replaced */
int  ebook_scan_count(void);
const char *ebook_scan_name(int idx);  /* NUL-terminated, no path */
/* Display name of the currently loaded source: "整卡" for the whole tree,
 * else the folder's basename. */
const char *ebook_current_src_name(void);

/* How the position of a freshly opened book was restored. Lets the UI say
 * whether the position is exactly where the reader left it or only an
 * approximation after the file changed. */
typedef enum {
    EBOOK_RESUME_NONE = 0,   /* nothing saved, or nothing usable: page 1 */
    EBOOK_RESUME_EXACT,      /* the saved offset still holds the same text */
    EBOOK_RESUME_DRIFT,      /* the text moved; re-located via its context */
    EBOOK_RESUME_PERCENT,    /* the text is gone; fell back to the percentage */
} ebook_resume_t;

bool ebook_open(int idx);              /* open book idx, position at page 1 */
void ebook_close(void);

/* Reading progress. After ebook_open() returns true, ebook_resume_percent()
 * is the byte progress (0..100) of the saved position the book was restored
 * to, or 0 when it opened at page 1, and ebook_resume_kind() says how that
 * position was found.
 *
 * ebook_progress_flush() persists the current position right away and returns
 * whether the card accepted it (debounced saves happen automatically on
 * flips); call it when leaving the reader page. ebook_save_failed() reports
 * the sticky result of the last save attempt. */
uint32_t ebook_resume_percent(void);
ebook_resume_t ebook_resume_kind(void);
bool ebook_save_failed(void);
bool ebook_progress_flush(void);
/* Erase all saved reading positions from the card, including the legacy v1
 * path-hash table. Use to wipe residual progress that resolves to the wrong
 * book. Safe to call at any time (serialized with the save path); the caller
 * should be on the UI task. */
void ebook_progress_clear_all(void);

/* Jump to the byte percentage (0..100) of the open book: backs up to the
 * nearest line start, lays the page that contains the target, takes the page
 * number from the page-start table when it already covers that offset and
 * arms a progress save. Returns false when no book is open.
 * 0% re-opens from the beginning. */
bool ebook_jump_percent(int pct);

/* Current page, 1-based. Until the background count has laid the book out,
 * this is the value carried over from the restore/jump; it is replaced by the
 * exact page number as soon as that walk lands. */
int  ebook_page(void);
int  ebook_page_count(void);           /* total pages; 0 until counted */
uint32_t ebook_count_version(void);    /* bumps when the count lands/changes */
int  ebook_percent(void);              /* byte progress 0..100 */

bool ebook_at_start(void);
bool ebook_at_end(void);
void ebook_page_flip(int dir);         /* +1 next page / -1 previous page */
const char *ebook_page_text(void);     /* current page, lines joined by '\n' */
