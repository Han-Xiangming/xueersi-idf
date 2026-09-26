/*
 * ebook_progress — persistent reading-progress store.
 *
 * Owns the on-card fixed-slot file (.progress.v2), the on-chip NVS mirror of the
 * last book, and the one-time v1 migration path. Extracted from ebook.c; the
 * reader engine only calls the API in ebook_progress.h. Design notes live in the
 * header and in docs/ebook.md 4.4.
 */
#define LOG_LOCAL_LEVEL ESP_LOG_INFO
#include "ebook_progress.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "ebook_prog";

/* On-card layouts are fixed: a change would silently misread every card written
 * by an older firmware. Force a version bump instead. */
_Static_assert(sizeof(eb_slot_t) == 72,
               "eb_slot_t changed: bump EBOOK_PROG_VERSION");
_Static_assert(sizeof(eb_v1_t) == 24,
               "eb_v1_t must match the v1 on-card record size");

/* Head/tail sample buffer for ebook_prog_fingerprint() (fingerprinting runs on
 * the caller's task only, so a single shared buffer is safe). */
static uint8_t s_fp_buf[EBOOK_FP_SAMPLE] EXT_RAM_BSS_ATTR;

/* On-card slot array mirrored into RAM. The in-memory array is the authoritative
 * copy; the file is only ever touched through slot_flush(). */
static eb_slot_t s_prog[EBOOK_PROG_MAX] EXT_RAM_BSS_ATTR;
static uint32_t s_seq;                   /* next slot sequence number */
static bool s_prog_loaded;               /* false until a load attempt ran */

/* On-chip NVS mirror handle (lazy open). */
static nvs_handle_t s_nvs_h = 0;

/* v1 table, loaded on demand for one-time migration into v2. */
static eb_v1_t s_v1[EBOOK_PROG_MAX] EXT_RAM_BSS_ATTR;
static int s_v1_count;
static bool s_v1_loaded;

/* Recursive mutex: serializes every access to s_prog[] and the .progress.v2
 * file across the UI task (open/flip/close/flush) and the background save task.
 * Recursive so prog_write() -> slot_flush() -> load() re-enter safely. Without
 * it, load() and slot_flush() race on the same file and array, silently
 * corrupting saved positions. */
static SemaphoreHandle_t s_prog_mux;

/* --- hashing / checksum ------------------------------------------------ */

uint32_t ebook_prog_fnv1a32(const char *s)
{
    uint32_t h = 0x811c9dc5u;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 0x01000193u;
    }
    return h;
}

uint64_t ebook_prog_fnv1a64(const uint8_t *d, size_t n, uint64_t h)
{
    for (size_t i = 0; i < n; i++) {
        h ^= d[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* 48-bit FNV-1a of the book path. Only used to recognise v1 entries during
 * migration; v2 addresses books by content fingerprint instead. */
uint64_t ebook_prog_fnv1a48(const char *s)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 0x100000001b3ULL;
    }
    return h & 0xFFFFFFFFFFFFULL;
}

/* CRC16-CCITT (poly 0x1021, init 0xFFFF), one per slot. */
uint16_t ebook_prog_crc16(const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    uint16_t c = 0xFFFF;
    while (n--) {
        c ^= (uint16_t)*p++ << 8;
        for (int i = 0; i < 8; i++) {
            c = (c & 0x8000u) ? (uint16_t)((c << 1) ^ 0x1021u)
                              : (uint16_t)(c << 1);
        }
    }
    return c;
}

/* --- content fingerprint ---------------------------------------------- */

/* FNV-1a64 over (size, first 1 KB, last 1 KB). Returns 0 only when the file
 * cannot be read at all. Uses its own FILE handle. */
uint64_t ebook_prog_fingerprint(const char *path, size_t size)
{
    uint8_t sz[8];
    memcpy(sz, &size, sizeof(size));         /* native order, stable per target */
    uint64_t h = ebook_prog_fnv1a64(sz, sizeof(sz), 0xcbf29ce484222325ULL);

    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return 0;
    }
    const size_t head = size < EBOOK_FP_SAMPLE ? size : EBOOK_FP_SAMPLE;
    if (head > 0 && fread(s_fp_buf, 1, head, fp) == head) {
        h = ebook_prog_fnv1a64(s_fp_buf, head, h);
    }
    if (size > EBOOK_FP_SAMPLE) {
        /* Tail sample: the last 1 KB, or whatever is left once head and tail
         * would otherwise overlap. */
        const size_t tail = (size >= 2 * EBOOK_FP_SAMPLE)
                                ? EBOOK_FP_SAMPLE
                                : size - EBOOK_FP_SAMPLE;
        if (fseeko(fp, (off_t)(size - tail), SEEK_SET) == 0 &&
            fread(s_fp_buf, 1, tail, fp) == tail) {
            h = ebook_prog_fnv1a64(s_fp_buf, tail, h);
        }
    }
    fclose(fp);
    return h;
}

/* --- v1 migration helpers (read-only, used once to seed v2) ----------- */

/* Load the v1 path-hash table once, for migration. We only ever read it: once
 * a v1 entry is migrated into v2 we drop it, so re-reading is unnecessary. */
static void progress_load_v1(void)
{
    s_v1_count = 0;
    s_v1_loaded = true;
    FILE *fp = fopen(EBOOK_PROG_FILE_V1, "rb");
    if (fp == NULL) {
        return;
    }
    uint32_t magic = 0, count = 0;
    if (fread(&magic, 4, 1, fp) == 1 && fread(&count, 4, 1, fp) == 1 &&
        magic == EBOOK_PROG_MAGIC_V1 && count > 0 && count <= EBOOK_PROG_MAX) {
        s_v1_count = (int)fread(s_v1, sizeof(eb_v1_t), count, fp);
    }
    fclose(fp);
}

/* v1 lookup by path hash + size. v1 stored no context bytes, so we never resume
 * straight from its offset — the caller seeds a v2 slot and lets
 * restore_position() validate it, degrading gracefully when the file changed. */
bool ebook_prog_v1_lookup(const char *path, size_t size, uint32_t *off)
{
    if (!s_v1_loaded) {
        progress_load_v1();
    }
    const uint64_t h = ebook_prog_fnv1a48(path);
    for (int i = 0; i < s_v1_count; i++) {
        if (s_v1[i].hash == h && s_v1[i].size == (uint32_t)size) {
            *off = s_v1[i].offset;
            return true;
        }
    }
    return false;
}

/* Remove a single v1 entry after it has been migrated, so it can never be
 * matched again (and the stale v1 file shrinks). */
void ebook_prog_v1_drop(const char *path)
{
    const uint64_t h = ebook_prog_fnv1a48(path);
    bool changed = false;
    for (int i = 0; i < s_v1_count; i++) {
        if (s_v1[i].hash == h) {
            for (int j = i; j < s_v1_count - 1; j++) {
                s_v1[j] = s_v1[j + 1];
            }
            s_v1_count--;
            changed = true;
            i--;
        }
    }
    if (!changed) {
        return;
    }
    FILE *fp = fopen(EBOOK_PROG_FILE_V1, "wb");
    if (fp == NULL) {
        return;
    }
    uint32_t magic = EBOOK_PROG_MAGIC_V1;
    uint32_t count = (uint32_t)s_v1_count;
    (void)fwrite(&magic, 4, 1, fp);
    (void)fwrite(&count, 4, 1, fp);
    if (s_v1_count > 0) {
        (void)fwrite(s_v1, sizeof(eb_v1_t), (size_t)s_v1_count, fp);
    }
    fclose(fp);
}

/* --- load / find ------------------------------------------------------ */

/* Load the v2 slot array. Slots whose CRC does not match are dropped, so a torn
 * sector costs only the books stored in it. The next sequence number continues
 * past the largest one on the card, which keeps "most recently read" ordering
 * monotonic across reboots. Recursive so prog_write() can call us while already
 * holding s_prog_mux. */
static void progress_load(void)
{
    xSemaphoreTakeRecursive(s_prog_mux, portMAX_DELAY);
    memset(s_prog, 0, sizeof(s_prog));
    s_prog_loaded = true;
    s_seq = 1;

    FILE *fp = fopen(EBOOK_PROG_FILE, "rb");
    if (fp == NULL) {
        ESP_LOGD(TAG, "no %s yet (errno %d)", EBOOK_PROG_FILE, errno);
        xSemaphoreGiveRecursive(s_prog_mux);
        return;                          /* no card yet, or first run */
    }
    uint32_t hdr[4] = { 0 };
    if (fread(hdr, 1, sizeof(hdr), fp) == sizeof(hdr) &&
        hdr[0] == EBOOK_PROG_MAGIC && hdr[1] == EBOOK_PROG_VERSION &&
        hdr[2] == (uint32_t)EBOOK_PROG_SLOT_SZ && hdr[3] == EBOOK_PROG_MAX) {
        for (int i = 0; i < EBOOK_PROG_MAX; i++) {
            eb_slot_t s;
            if (fread(&s, EBOOK_PROG_SLOT_SZ, 1, fp) != 1) {
                break;                   /* truncated tail: keep what we have */
            }
            if (s.fp != 0 && s.crc == ebook_prog_crc16(&s, offsetof(eb_slot_t, crc))) {
                s_prog[i] = s;
                if (s.seq >= s_seq) {
                    s_seq = s.seq + 1;
                }
            }
        }
    }
    fclose(fp);
    xSemaphoreGiveRecursive(s_prog_mux);
}

void ebook_prog_reload(void)
{
    progress_load();
}

static void ensure_loaded(void)
{
    if (!s_prog_loaded) {
        progress_load();                 /* recursive mutex: safe from anywhere */
    }
}

static int slot_find(uint64_t fp)
{
    for (int i = 0; i < EBOOK_PROG_MAX; i++) {
        if (s_prog[i].fp == fp) {
            return i;
        }
    }
    return -1;
}

/* Fallback lookup by path hash, used when the content fingerprint changed (e.g.
 * a re-encode / regeneration that shifted the head or tail sample) but the
 * filename is unchanged. Matches at most one book; a path-hash collision can
 * only cause a wrong restore attempt, which restore_position() still validates
 * against the live file. */
static int slot_find_path(uint32_t path_h)
{
    for (int i = 0; i < EBOOK_PROG_MAX; i++) {
        if (s_prog[i].fp != 0 && s_prog[i].path_h == path_h) {
            return i;
        }
    }
    return -1;
}

bool ebook_prog_lookup(uint64_t fp, eb_slot_t *out)
{
    ensure_loaded();
    const int i = slot_find(fp);
    if (i < 0) {
        return false;
    }
    *out = s_prog[i];
    return true;
}

bool ebook_prog_lookup_path(uint32_t path_h, eb_slot_t *out)
{
    ensure_loaded();
    const int i = slot_find_path(path_h);
    if (i < 0) {
        return false;
    }
    *out = s_prog[i];
    return true;
}

/* --- on-chip NVS mirror of the most-recently-read book ---------------- */

/* Open the NVS handle on first use; returns false if NVS is unavailable. */
static bool nvs_mirror_ensure(void)
{
    if (s_nvs_h != 0) {
        return true;
    }
    return nvs_open(EBOOK_NVS_NS, NVS_READWRITE, &s_nvs_h) == ESP_OK;
}

/* Persist the current snapshot to NVS. Called on every successful card save, so
 * the mirror always tracks the latest position — and, crucially, it is also
 * written when the card write fails, acting as the "pending补写" copy that
 * survives an SD removal or corruption until the card is available again. */
static void nvs_mirror_save(const eb_snap_t *snap)
{
    if (snap->fp == 0 || !nvs_mirror_ensure()) {
        return;
    }
    eb_nvs_mirror_t m;
    memset(&m, 0, sizeof(m));
    m.fp = snap->fp;
    m.path_h = ebook_prog_fnv1a32(snap->path);
    m.off = snap->off;
    m.pct = snap->pct;
    snprintf(m.name, sizeof(m.name), "%s", snap->name);
    memcpy(m.ctx, snap->ctx, EBOOK_CTX_LEN);
    if (nvs_set_blob(s_nvs_h, EBOOK_NVS_MIRROR, &m, sizeof(m)) == ESP_OK) {
        nvs_commit(s_nvs_h);
    }
}

/* Load the NVS mirror into a slot-shaped struct for restore_position().
 * Returns true when a valid (fp != 0) record exists. */
bool ebook_prog_mirror_load(eb_slot_t *out)
{
    if (!nvs_mirror_ensure()) {
        return false;
    }
    eb_nvs_mirror_t m;
    size_t len = sizeof(m);
    if (nvs_get_blob(s_nvs_h, EBOOK_NVS_MIRROR, &m, &len) != ESP_OK) {
        return false;
    }
    if (m.fp == 0) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->fp = m.fp;
    out->path_h = m.path_h;
    out->off = m.off;
    out->pct = m.pct;
    snprintf(out->name, sizeof(out->name), "%s", m.name);
    memcpy(out->ctx, m.ctx, EBOOK_CTX_LEN);
    return true;
}

/* --- write ------------------------------------------------------------ */

/* A free slot, else the one with the oldest sequence number (least recently
 * read). */
static int slot_alloc(void)
{
    int oldest = 0;
    for (int i = 0; i < EBOOK_PROG_MAX; i++) {
        if (s_prog[i].fp == 0) {
            return i;
        }
        if (s_prog[i].seq < s_prog[oldest].seq) {
            oldest = i;
        }
    }
    return oldest;
}

/* Create an empty v2 file (header + zeroed slot array). */
static bool progress_create(void)
{
    FILE *fp = fopen(EBOOK_PROG_FILE, "wb");
    if (fp == NULL) {
        ESP_LOGW(TAG, "cannot create %s (errno %d)", EBOOK_PROG_FILE, errno);
        return false;
    }
    const uint32_t hdr[4] = { EBOOK_PROG_MAGIC, EBOOK_PROG_VERSION,
                              (uint32_t)EBOOK_PROG_SLOT_SZ, EBOOK_PROG_MAX };
    eb_slot_t empty;
    memset(&empty, 0, sizeof(empty));

    bool ok = fwrite(hdr, 1, sizeof(hdr), fp) == sizeof(hdr);
    for (int i = 0; ok && i < EBOOK_PROG_MAX; i++) {
        ok = fwrite(&empty, EBOOK_PROG_SLOT_SZ, 1, fp) == 1;
    }
    /* fclose() is what commits the data: for FATFS it runs f_close(), which
     * writes back the FAT chain and the directory entry. No explicit fsync:
     * fsync() is a weak console-only stub in some esp_stdio configurations and
     * then fails with EBADF for any real file. */
    if (fclose(fp) != 0) {
        ok = false;
    }
    ESP_LOGI(TAG, "created %s (%u B) ok=%d", EBOOK_PROG_FILE,
             (unsigned)(EBOOK_PROG_HDR_SZ + EBOOK_PROG_MAX * EBOOK_PROG_SLOT_SZ),
             (int)ok);
    return ok;
}

/* Write one slot back to the card in place: a single positioned write, no temp
 * file and no rename, so a power cut can only damage the slot being written.
 * Returns false when the write or the close failed. */
static bool slot_flush(int idx)
{
    xSemaphoreTakeRecursive(s_prog_mux, portMAX_DELAY);  /* re-entrant */
    FILE *fp = fopen(EBOOK_PROG_FILE, "r+b");
    if (fp == NULL && progress_create()) {
        fp = fopen(EBOOK_PROG_FILE, "r+b");
    }
    if (fp == NULL) {
        ESP_LOGW(TAG, "cannot open %s (errno %d)", EBOOK_PROG_FILE, errno);
        return false;
    }
    s_prog[idx].crc = ebook_prog_crc16(&s_prog[idx], offsetof(eb_slot_t, crc));
    bool ok = fseeko(fp, (off_t)(EBOOK_PROG_HDR_SZ +
                                 (size_t)idx * EBOOK_PROG_SLOT_SZ),
                     SEEK_SET) == 0 &&
              fwrite(&s_prog[idx], EBOOK_PROG_SLOT_SZ, 1, fp) == 1;
    if (fclose(fp) != 0) {                /* commits the write */
        ok = false;
    }
    if (!ok) {
        ESP_LOGW(TAG, "slot %d write failed (errno %d)", idx, errno);
    }
    xSemaphoreGiveRecursive(s_prog_mux);
    return ok;
}

/* Remember a position. Rewrites exactly one slot; the in-memory array is the
 * authoritative copy and the file is only ever touched through slot_flush(). */
static bool prog_write(uint64_t fp, const char *path, uint32_t off,
                       uint8_t pct, const char *name, const char *ctx)
{
    if (fp == 0 || path[0] == '\0') {
        return false;                    /* nothing identifiable to save */
    }
    /* Held by ebook_prog_save() already (recursive mutex): re-entering here is
     * safe and keeps slot_flush()'s file access serialized with load(). */
    xSemaphoreTakeRecursive(s_prog_mux, portMAX_DELAY);
    if (!s_prog_loaded) {
        progress_load();                 /* recursive; no deadlock */
    }
    int idx = slot_find(fp);
    if (idx < 0) {
        idx = slot_alloc();
        memset(&s_prog[idx], 0, sizeof(s_prog[idx]));
        s_prog[idx].fp = fp;
    }
    s_prog[idx].path_h = ebook_prog_fnv1a32(path);
    s_prog[idx].off = off;
    s_prog[idx].pct = pct;
    s_prog[idx].seq = s_seq++;
    snprintf(s_prog[idx].name, sizeof(s_prog[idx].name), "%s", name);
    memcpy(s_prog[idx].ctx, ctx, EBOOK_CTX_LEN);
    bool ok = slot_flush(idx);
    xSemaphoreGiveRecursive(s_prog_mux);
    return ok;
}

bool ebook_prog_save(const eb_snap_t *snap)
{
    if (snap->fp == 0 || snap->path[0] == '\0') {
        return false;                    /* nothing identifiable to save */
    }
    /* Serialize with load() and any other writer: the background save task and
     * the UI task (flush/close) can both enter here, and must not run against a
     * concurrent load() rebuilding s_prog from the card. Recursive so
     * prog_write() -> slot_flush() -> progress_load() re-enter safely. Block
     * (don't drop): the caller surfaces this return value to the user, so a
     * discarded save would lose the latest position silently. */
    if (xSemaphoreTakeRecursive(s_prog_mux, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    const bool ok = prog_write(snap->fp, snap->path, snap->off, snap->pct,
                               snap->name, snap->ctx);
    nvs_mirror_save(snap);   /* mirror latest position regardless of the card
                              * result: on SD success it is the redundancy, on
                              * SD failure it is the pending补写 copy. */
    ESP_LOGD(TAG, "save off=%u pct=%u%% fp=%08X%08X ok=%d",
             (unsigned)snap->off, (unsigned)snap->pct,
             (unsigned)(uint32_t)(snap->fp >> 32),
             (unsigned)(uint32_t)snap->fp, (int)ok);

    xSemaphoreGiveRecursive(s_prog_mux);
    if (!ok) {
        ESP_LOGW(TAG, "progress save failed for '%s'", snap->path);
    }
    return ok;
}

/* --- init / clear ----------------------------------------------------- */

void ebook_prog_init(void)
{
    s_prog_mux = xSemaphoreCreateRecursiveMutex();
    s_prog_loaded = false;
    s_v1_loaded = false;
}

/* Erase every saved reading position from the card, including the legacy v1
 * path-hash table. v1 is the usual source of "resumes to the wrong book": it
 * keys only on path hash + file size, so two books that happen to share a size
 * can swap positions. Wiping both files removes all residual progress. The
 * in-memory array is reset too, under the same mutex as the save path, so a
 * concurrent save/load cannot race the wipe. */
void ebook_prog_clear_all(void)
{
    xSemaphoreTakeRecursive(s_prog_mux, portMAX_DELAY);
    remove(EBOOK_PROG_FILE);
    remove(EBOOK_PROG_FILE_V1);
    memset(s_prog, 0, sizeof(s_prog));
    s_prog_loaded = true;        /* memory is now the authoritative empty set */
    s_seq = 1;
    s_v1_loaded = true;
    s_v1_count = 0;
    xSemaphoreGiveRecursive(s_prog_mux);
    if (nvs_mirror_ensure()) {
        nvs_erase_key(s_nvs_h, EBOOK_NVS_MIRROR);
        nvs_commit(s_nvs_h);
    }
    ESP_LOGI(TAG, "cleared all (v2 + v1 + NVS mirror)");
}
