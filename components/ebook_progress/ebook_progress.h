/*
 * ebook_progress: persistent reading-progress store for the Xiaomiao TXT reader.
 *
 * This component is the single owner of the on-card progress file and the
 * on-chip NVS mirror. It was extracted out of ebook.c so the persistence layer
 * has one decoupled, testable API (see docs/ebook.md 4.4). The reader engine in
 * ebook.c keeps the scheduling (debounce / background save task / 4-level resume
 * fallback) and only talks to this module through the functions below.
 *
 * File format v2: a fixed-size array of EBOOK_PROG_MAX slots. A book is
 * addressed by a CONTENT fingerprint (file size + a head/tail sample), not by
 * its path, so renaming, moving or re-copying a book keeps its reading
 * position. Each slot carries its own CRC16 and a monotonic sequence number:
 * saving one book is a single positioned write of one slot, and a corrupt slot
 * costs only that one book. There is deliberately no global checksum — it would
 * have to be rewritten on every save, defeating the single-slot write.
 *
 * A debounced save runs after the last page flip; ebook_close() /
 * ebook_progress_flush() save synchronously. See the ebook component for timing.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The progress file sits next to the books. Keep this identical to EBOOK_ROOT
 * (components/app/ebook/ebook.h) so progress follows the card. */
#ifndef EBOOK_PROG_ROOT
#define EBOOK_PROG_ROOT "/sdcard/eBook"
#endif

/* --- on-card v2 format ------------------------------------------------ */
#define EBOOK_PROG_FILE      EBOOK_PROG_ROOT "/.progress.v2"
#define EBOOK_PROG_MAGIC     0x46504245u   /* "EBPF" */
#define EBOOK_PROG_VERSION   2
#define EBOOK_PROG_HDR_SZ    16
#define EBOOK_PROG_MAX       512           /* remembered books */
#define EBOOK_CTX_LEN        16            /* context bytes kept at the offset */
#define EBOOK_PROG_NAME_LEN  32            /* stored display name (UTF-8) */
#define EBOOK_FP_SAMPLE      1024          /* head / tail bytes per fingerprint */
#define EBOOK_PROG_PATH_MAX  320           /* path buffer inside a snapshot */

/* v1 file: read-only (we only ever read it to migrate into v2). */
#define EBOOK_PROG_FILE_V1   EBOOK_PROG_ROOT "/.progress"
#define EBOOK_PROG_MAGIC_V1  0x47504245u   /* "EBPG" */

/* On-chip NVS mirror of the most-recently-read book. */
#define EBOOK_NVS_NS     "ebook_nvs"
#define EBOOK_NVS_MIRROR "mirror"

/* One remembered book. `fp` identifies the book; `off` is the byte offset of
 * the page the reader stopped on (always a line start); `ctx` is the raw text
 * found at that offset, used to re-locate the position when the file was edited
 * ahead of it; `pct` is the last-resort anchor. */
typedef struct {
    uint64_t fp;                        /* content fingerprint; 0 = empty    */
    uint32_t path_h;                    /* path FNV-1a32 (list display only) */
    uint32_t off;                       /* page-start byte offset            */
    uint32_t seq;                       /* monotonic write sequence          */
    char     ctx[EBOOK_CTX_LEN];        /* raw bytes at `off`                */
    char     name[EBOOK_PROG_NAME_LEN]; /* display name, UTF-8               */
    uint8_t  pct;                       /* off * 100 / size                  */
    uint8_t  flags;                     /* reserved                          */
    uint16_t crc;                       /* CRC16-CCITT over the first 70 B   */
} eb_slot_t;
#define EBOOK_PROG_SLOT_SZ  sizeof(eb_slot_t)

/* NVS mirror record (one book: the open one). Mirrors exactly one book; the
 * card stays the authoritative per-book store, so this never breaks "progress
 * follows the book". NVS wear-levels its flash, so writing on every save is
 * cheap. */
typedef struct {
    uint64_t fp;                         /* content fingerprint (0 = empty)   */
    uint32_t path_h;                     /* path hash, name-based fallback id */
    uint32_t off;                        /* page-start byte offset            */
    uint8_t  pct;                        /* off * 100 / size                  */
    char     name[EBOOK_PROG_NAME_LEN];  /* display name, UTF-8               */
    char     ctx[EBOOK_CTX_LEN];         /* raw bytes at `off` (anchor)       */
} eb_nvs_mirror_t;

/* v1 entry (path-hash MRU table), read only for migration into v2. The layout
 * must stay byte-identical to the one the old firmware wrote. */
typedef struct {
    uint64_t hash;                      /* 48-bit FNV-1a of the book path    */
    uint32_t offset;
    uint32_t page;
    uint32_t size;
} eb_v1_t;

/* Write payload: everything needed to persist one book position. Filled in by
 * the reader engine (ebook.c) and handed to ebook_prog_save(). */
typedef struct {
    uint64_t fp;
    uint32_t off;
    uint8_t  pct;
    char     path[EBOOK_PROG_PATH_MAX];
    char     name[EBOOK_PROG_NAME_LEN];
    char     ctx[EBOOK_CTX_LEN];
} eb_snap_t;

/* --- hashing / checksum (also used by the reader for fingerprinting) ---- */
uint32_t ebook_prog_fnv1a32(const char *s);
uint64_t ebook_prog_fnv1a64(const uint8_t *d, size_t n, uint64_t h);
uint64_t ebook_prog_fnv1a48(const char *s);
uint16_t ebook_prog_crc16(const void *data, size_t n);

/* --- API ------------------------------------------------------------- */

/* Content fingerprint of a book: FNV-1a64 over (size, first 1 KB, last 1 KB).
 * Path-independent on purpose: renaming, moving to another folder or re-copying
 * the card all keep the same fingerprint, so the reading position follows the
 * book instead of following its path. Returns 0 when the file cannot be read at
 * all — 0 also marks an empty slot, so such a book is simply never remembered.
 * Uses its own FILE handle so the caller's reader window stays untouched. */
uint64_t ebook_prog_fingerprint(const char *path, size_t size);

/* Create the file-cache mutex. Call once at boot (after the SD card is mounted
 * and NVS is initialised). The v2 table is loaded lazily on first access. */
void ebook_prog_init(void);

/* Rebuild the in-memory slot cache from the card. Call on book open so a card
 * swap or an external edit takes effect immediately. Safe to call repeatedly. */
void ebook_prog_reload(void);

/* Look up a saved slot by content fingerprint / path hash. Returns true and
 * fills *out when a non-empty slot matches. */
bool ebook_prog_lookup(uint64_t fp, eb_slot_t *out);
bool ebook_prog_lookup_path(uint32_t path_h, eb_slot_t *out);

/* Load the on-chip NVS mirror of the last book (if any) into *out. Returns true
 * when a valid (fp != 0) record exists. */
bool ebook_prog_mirror_load(eb_slot_t *out);

/* Persist a position. Rewrites exactly one slot on the card and updates the NVS
 * mirror. The mirror is written even when the card write fails, acting as the
 * "pending补写" copy that survives an SD removal or corruption until the card is
 * available again. Returns whether the card accepted the slot write. No-ops
 * (returns false) when fp is 0 or the path is empty. */
bool ebook_prog_save(const eb_snap_t *snap);

/* v1 migration helpers (read-only over the legacy .progress file). */
bool ebook_prog_v1_lookup(const char *path, size_t size, uint32_t *off);
void ebook_prog_v1_drop(const char *path);

/* Erase all saved reading positions from the card (v2 + v1) and the NVS mirror.
 * Safe to call at any time (serialized with the save path); the caller should be
 * on the UI task. */
void ebook_prog_clear_all(void);
