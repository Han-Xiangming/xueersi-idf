/*
 * Hardware layer: SD card over SDSPI.
 * See hw_sd.h.
 */
#include <string.h>

#include "board.h"
#include "hw/hw_sd.h"

#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "sdmmc_cmd.h"
#include "sys/param.h"

static const char *TAG = "hw_sd";

static sdmmc_card_t *s_sd_card;
static bool s_mounted;
static char s_name[24];
static uint32_t s_mb;
static esp_err_t s_last_err = ESP_ERR_NOT_FOUND;

static void set_unmounted_state(void)
{
    s_mounted = false;
    s_sd_card = NULL;
    memset(s_name, 0, sizeof(s_name));
    memcpy(s_name, "NO CARD", sizeof("NO CARD"));
    s_mb = 0;
    s_last_err = ESP_ERR_NOT_FOUND;
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
    mount_config.max_files = 3;

    s_last_err = esp_vfs_fat_sdspi_mount("/sdcard",
                                         &host,
                                         &slot_config,
                                         &mount_config,
                                         &s_sd_card);
    if (s_last_err == ESP_OK && s_sd_card) {
        s_mounted = true;
        memset(s_name, 0, sizeof(s_name));
        memcpy(s_name,
               s_sd_card->cid.name,
               MIN(sizeof(s_sd_card->cid.name), sizeof(s_name) - 1));
        s_mb = (uint32_t)(((uint64_t)s_sd_card->csd.capacity * s_sd_card->csd.sector_size) / (1024 * 1024));
    }
    else {
        s_mounted = false;
        s_sd_card = NULL;
        memset(s_name, 0, sizeof(s_name));
        memcpy(s_name, "NO CARD", sizeof("NO CARD"));
        s_mb = 0;
    }
}

void hw_sd_unmount(void)
{
    if (s_mounted && s_sd_card) {
        esp_err_t err = esp_vfs_fat_sdcard_unmount("/sdcard", s_sd_card);
        if (err != ESP_OK) {
            s_last_err = err;
            return;
        }
    }
    set_unmounted_state();
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
