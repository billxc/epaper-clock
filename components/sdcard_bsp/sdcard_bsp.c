// SDMMC pin assignments adapted from the Waveshare board example.
#include "sdcard_bsp.h"
#include "fat_frame_cache.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"

#if !FF_FS_EXFAT || FF_USE_LFN != 3
#error "Frame cache requires SDK FatFs exFAT with heap LFN buffers"
#endif
_Static_assert(sizeof(FSIZE_t) == 8, "FatFs consumers must use the 64-bit exFAT ABI");
_Static_assert(FF_VOLUMES <= 10, "FatFs numeric drive paths must fit one digit");

static const char *TAG = "sd_cache";
static sdmmc_card_t card;
static FATFS filesystem;
static fat_frame_cache_t cache;
static BYTE pdrv = FF_DRV_NOT_USED;
static char drive[3];
static bool host_initialized;
static bool disk_registered;
static bool filesystem_registered;
static bool cache_open_attempted;
// Do not retry absent/faulty media at every menu redraw. Card changes require
// poweroff; a new boot may try again, while successful mounts can be reopened.
static esp_err_t mount_failure;
// Once glass may have changed, a failed invalidate must not permit a stale
// on-card header to be trusted later in this boot (even after remount).
static bool baseline_trusted = true;

esp_err_t cache_mount(void)
{
    // Lifecycle operations are serialized by the caller (no VFS is installed).
    if (cache.opened) return ESP_OK;
    if (mount_failure != ESP_OK) return mount_failure;
    if (host_initialized || disk_registered || filesystem_registered)
        return ESP_ERR_INVALID_STATE;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT | SDMMC_HOST_FLAG_4BIT;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = 16; slot.cmd = 17;
    slot.d0 = 15; slot.d1 = 7; slot.d2 = 8; slot.d3 = 18;
    esp_err_t result = sdmmc_host_init();
    if (result != ESP_OK) {
        mount_failure = result;
        return result;
    }
    host_initialized = true;
    result = sdmmc_host_init_slot(host.slot, &slot);
    if (result != ESP_OK) goto fail;
    result = sdmmc_card_init(&host, &card);
    if (result != ESP_OK) goto fail;
    if (card.csd.sector_size != 512) {
        result = ESP_ERR_NOT_SUPPORTED;
        goto fail;
    }

    result = ff_diskio_get_drive(&pdrv);
    if (result != ESP_OK) goto fail;
    if (pdrv >= FF_VOLUMES) {
        result = ESP_ERR_NOT_SUPPORTED;
        goto fail;
    }
    ff_diskio_register_sdmmc(pdrv, &card);
    disk_registered = true;
    drive[0] = (char)('0' + pdrv);
    drive[1] = ':';
    drive[2] = '\0';
    // SDK VolToPart defaults to identity mapping and auto-detected partition.
    // f_mount installs the object even when immediate mounting fails.
    filesystem_registered = true;
    FRESULT fr = f_mount(&filesystem, drive, 1);
    if (fr != FR_OK) {
        ESP_LOGW(TAG, "FatFs mount failed (%d); leaving card unmodified", fr);
        result = fr == FR_NO_FILESYSTEM ? ESP_ERR_NOT_SUPPORTED : ESP_FAIL;
        goto fail;
    }

    char path[] = "0:/" FRAME_CACHE_FILE_NAME;
    path[0] = drive[0];
    cache_open_attempted = true;
    if (!fat_frame_cache_open(&cache, path)) {
        ESP_LOGW(TAG, "Frame cache unavailable; use full refresh");
        result = ESP_FAIL;
        goto fail;
    }
    return ESP_OK;

fail:
    mount_failure = result;
    cache_unmount();
    return result;
}

bool cache_read(uint8_t *frame, size_t size)
{
    return baseline_trusted && cache.opened && fat_frame_cache_read(&cache, frame, size);
}

bool cache_write(const uint8_t *frame, size_t size)
{
    bool written = cache.opened && fat_frame_cache_write(&cache, frame, size);
    baseline_trusted = written;
    return written;
}

void cache_invalidate(void)
{
    // Read eligibility is not proof of a durably invalid on-card header.
    // Only the backend's successful-sync state may deduplicate this write.
    baseline_trusted = false;
    if (cache.opened && !fat_frame_cache_invalidate(&cache))
        ESP_LOGW(TAG, "Unable to invalidate frame cache");
}

void cache_unmount(void)
{
    if (cache_open_attempted) {
        fat_frame_cache_close(&cache);
        cache_open_attempted = false;
    }
    if (filesystem_registered) {
        FRESULT fr = f_mount(NULL, drive, 0);
        if (fr != FR_OK) {
            // Do not detach disk/host while FatFs may still reference them.
            ESP_LOGW(TAG, "FatFs unmount failed (%d)", fr);
            return;
        }
        filesystem_registered = false;
    }
    if (disk_registered) {
        ff_diskio_unregister(pdrv);
        disk_registered = false;
    }
    pdrv = FF_DRV_NOT_USED;
    if (host_initialized) {
        esp_err_t result = sdmmc_host_deinit();
        if (result == ESP_OK) host_initialized = false;
        else ESP_LOGW(TAG, "SDMMC deinit failed (%s)", esp_err_to_name(result));
    }
}
