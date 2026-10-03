/*
 * Hardware layer: SD card over SDSPI.
 * See sd.h.
 */
#include <string.h>

#include "board_config.h"
#include "sd.h"

#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "sdmmc_cmd.h"
#include "sys/param.h"

static const char *TAG = "hw_sd";

/* Pre-allocated bounce buffer for unaligned sector I/O.
 *
 * ESP32's SPI DMA can only touch internal DRAM, and the SDSPI host's alignment
 * check rejects any destination that is not 4-byte aligned. FATFS hands us such
 * buffers regularly, so every unaligned read/write fell into sdmmc_cmd.c's
 * "allocate a temporary DMA buffer" branch, which does
 * heap_caps_malloc(size, MALLOC_CAP_DMA) on EVERY sector access and frees it
 * right after. That is fine until the internal DMA pool runs dry: the malloc
 * folds the request down to a single 512-byte sector, still fails, and the
 * access aborts with
 *     sdmmc_cmd: allocate_dma_buf: not enough mem, err=0x101
 * which is what wedged playback once the Bluetooth controller took its share
 * of internal DRAM.
 *
 * Handing the driver one long-lived DMA buffer instead moves that allocation
 * to mount time, when the pool still has room, and makes every later sector
 * access allocation-free. The size is a throughput knob only: the driver
 * clamps the tail chunk of a request to the remaining block count. */
#define SD_DMA_BUF_BYTES         2048
#define SD_DMA_MAX_CHUNK_BLOCKS  4

static sdmmc_card_t *s_sd_card;
static void *s_sd_dma_buf;
static bool s_mounted;
static char s_name[24];
static uint32_t s_mb;
static esp_err_t s_last_err = ESP_ERR_NOT_FOUND;

/* Attach the bounce buffer to the mounted card. Best effort: on failure the
 * driver keeps using per-access temporary buffers, i.e. the old behaviour. */
static void sd_attach_dma_buf(void)
{
    if (s_sd_card == NULL || s_sd_dma_buf != NULL) {
        return;
    }

    const size_t sector = s_sd_card->csd.sector_size;
    if (sector == 0) {
        return;
    }

    void *buf = heap_caps_aligned_alloc(4, SD_DMA_BUF_BYTES, MALLOC_CAP_DMA);
    if (buf == NULL) {
        ESP_LOGW(TAG, "no internal DMA memory for the %d B bounce buffer; "
                      "unaligned sector I/O will allocate per access",
                 SD_DMA_BUF_BYTES);
        return;
    }

    size_t blocks = heap_caps_get_allocated_size(buf) / sector;
    if (blocks > SD_DMA_MAX_CHUNK_BLOCKS) {
        blocks = SD_DMA_MAX_CHUNK_BLOCKS;
    }
    if (blocks == 0) {
        free(buf);
        return;
    }

    s_sd_dma_buf                   = buf;
    s_sd_card->host.dma_aligned_buffer = buf;
    s_sd_card->host.unaligned_multi_block_rw_max_chunk_size = blocks;
    ESP_LOGI(TAG, "SD bounce buffer: %u B internal DMA, %u sector(s) per chunk",
             (unsigned)heap_caps_get_allocated_size(buf), (unsigned)blocks);
}

void hw_sd_try_mount(void)
{
    if (s_mounted) {
        return;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = LCD_HOST;
    host.max_freq_khz = SD_SPI_MAX_FREQ_KHZ;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.host_id = LCD_HOST;
    slot_config.gpio_cs = PIN_NUM_SD_CS;
    slot_config.wait_for_miso = 20;

    esp_vfs_fat_mount_config_t mount_config = VFS_FAT_MOUNT_DEFAULT_CONFIG();
    mount_config.format_if_mount_failed = false;
    /* Open-file budget: the player holds its track open, the ebook reader
     * its book, and the ebook count task its own handle — that is already
     * the old limit of 3, so any transient opendir/fopen("w") (scans, cache
     * read/write) failed while playing + reading, breaking track changes.
     * 8 keeps every long-lived handle plus all transient opens under the
     * FATFS table limit. */
    mount_config.max_files = 8;

    s_last_err = esp_vfs_fat_sdspi_mount("/sdcard",
                                         &host,
                                         &slot_config,
                                         &mount_config,
                                         &s_sd_card);
    if (s_last_err == ESP_OK && s_sd_card) {
        s_mounted = true;
        s_last_err = ESP_OK;
        sd_attach_dma_buf();
        memset(s_name, 0, sizeof(s_name));
        memcpy(s_name,
               s_sd_card->cid.name,
               MIN(sizeof(s_sd_card->cid.name), sizeof(s_name) - 1));
        s_mb = (uint32_t)(((uint64_t)s_sd_card->csd.capacity * s_sd_card->csd.sector_size) / (1024 * 1024));
        ESP_LOGI(TAG, "SD mounted: %s, %lu MB", s_sd_card->cid.name, (unsigned long)s_mb);
    }
    else {
        s_mounted = false;
        s_sd_card = NULL;
        memset(s_name, 0, sizeof(s_name));
        memcpy(s_name, "NO CARD", sizeof("NO CARD"));
        s_mb = 0;
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(s_last_err));
    }
}

bool hw_sd_is_mounted(void)
{
    return s_mounted;
}

const char *hw_sd_name(void)
{
    return s_name;
}

uint32_t hw_sd_mb(void)
{
    return s_mb;
}

esp_err_t hw_sd_last_err(void)
{
    return s_last_err;
}
