// SDMMC pin assignments adapted from the Waveshare board example.
#include "sdcard_bsp.h"
#include "ext4_cache.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"

static sdmmc_card_t card;
static ext4_cache_t cache;
static bool host_initialized;

static bool read_sectors(void *context, uint64_t lba, size_t count, void *data)
{
    return lba <= SIZE_MAX &&
        sdmmc_read_sectors(context, data, (size_t)lba, count) == ESP_OK;
}

static bool write_sectors(void *context, uint64_t lba, size_t count, const void *data)
{
    return lba <= SIZE_MAX &&
        sdmmc_write_sectors(context, data, (size_t)lba, count) == ESP_OK;
}

esp_err_t cache_mount(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT | SDMMC_HOST_FLAG_4BIT;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = 16; slot.cmd = 17;
    slot.d0 = 15; slot.d1 = 7; slot.d2 = 8; slot.d3 = 18;
    esp_err_t result = sdmmc_host_init();
    if (result != ESP_OK) return result;
    host_initialized = true;
    result = sdmmc_host_init_slot(host.slot, &slot);
    if (result == ESP_OK) result = sdmmc_card_init(&host, &card);
    if (result == ESP_OK && (card.csd.sector_size != EXT4_CACHE_SECTOR_SIZE ||
        !ext4_cache_open(&cache, read_sectors, write_sectors, &card, card.csd.capacity)))
        result = ESP_ERR_NOT_SUPPORTED;
    if (result != ESP_OK) cache_unmount();
    return result;
}

bool cache_read(uint8_t *frame, size_t size)
{
    return ext4_cache_read(&cache, frame, size);
}

bool cache_write(const uint8_t *frame, size_t size)
{
    return ext4_cache_write(&cache, frame, size);
}

void cache_invalidate(void)
{
    if (cache.opened && !ext4_cache_invalidate(&cache))
        ESP_LOGW("sd_cache", "Unable to invalidate frame cache");
}

void cache_unmount(void)
{
    cache.opened = false;
    if (host_initialized) {
        sdmmc_host_deinit();
        host_initialized = false;
    }
}
