// SDMMC pin assignments adapted from the Waveshare board example.
#include "sdcard_bsp.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include <stdio.h>

#define CACHE_PATH "/sdcard/epaper-clock-v1.bin"
static sdmmc_card_t *card;

esp_err_t cache_mount(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false, .max_files = 1,
        .allocation_unit_size = 32 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = 16; slot.cmd = 17;
    slot.d0 = 15; slot.d1 = 7; slot.d2 = 8; slot.d3 = 18;
    return esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mount, &card);
}

bool cache_read(uint8_t *frame, size_t size)
{
    if (!card) return false;
    FILE *file = fopen(CACHE_PATH, "rb");
    if (!file) return false;
    bool ok = fread(frame, 1, size, file) == size && fgetc(file) == EOF;
    fclose(file);
    return ok;
}

bool cache_write(const uint8_t *frame, size_t size)
{
    if (!card) return false;
    FILE *file = fopen(CACHE_PATH, "wb");
    if (!file) return false;
    bool ok = fwrite(frame, 1, size, file) == size;
    if (fclose(file) != 0) ok = false;
    if (!ok) remove(CACHE_PATH);
    return ok;
}

void cache_unmount(void)
{
    if (card) {
        ESP_ERROR_CHECK(esp_vfs_fat_sdcard_unmount("/sdcard", card));
        card = NULL;
    }
}
