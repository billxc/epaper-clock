// SDMMC pin assignments adapted from the Waveshare board example.
#include "sdcard_bsp.h"
#include "fat_frame_cache.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include <stdio.h>
#include <string.h>

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
static bool host_initialized, disk_registered, filesystem_registered;
static bool baseline_trusted = true;
static sdmmc_cid_t last_cid;
static bool have_cid, card_initialized;
static int64_t retry_after, invalidate_after;
static uint32_t diagnostic_sequence;
#define RETRY_US 5000000LL

/* There is no wired CD GPIO on the published board schematic (TF1 CD/9
 * shares GND). CMD13 alone cannot prove that a replacement card has the same
 * identity. Instead, NO FIL/FATFS/card session survives a public operation.
 * Card changes are supported only BETWEEN synchronous SD operations, never
 * during IO. All entry points belong to the serial UI owner, no worker task. */
void cache_unmount(void)
{
    // Never flush an old handle during detach/recovery. Successful operations
    // already synced and closed it; failures must discard their dirty buffers.
    fat_frame_cache_abandon(&cache);
    if (filesystem_registered) {
#if FF_USE_DYN_BUFFER
        // SDK allocates win before validating the boot sector, but f_mount's
        // detach frees it only for fs_type != 0. Handle failed initial mounts.
        if (!filesystem.fs_type && filesystem.win) {
            ff_memfree(filesystem.win);
            filesystem.win = NULL;
        }
#endif
        FRESULT fr = f_mount(NULL, drive, 0); // SDK: unregister, no disk writes
        if (fr != FR_OK) {
            ESP_LOGW(TAG, "FatFs detach failed (%d); refusing host reuse", fr);
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

static void failed(void)
{
    baseline_trusted = false;
    retry_after = esp_timer_get_time() + RETRY_US;
    cache_unmount();
}

static esp_err_t mount_volume(void)
{
    card_initialized = false;
    if (esp_timer_get_time() < retry_after) return ESP_ERR_INVALID_STATE;
    if (host_initialized || disk_registered || filesystem_registered) {
        cache_unmount();
        if (host_initialized || disk_registered || filesystem_registered)
            return ESP_ERR_INVALID_STATE;
    }
    memset(&card, 0, sizeof(card));
    memset(&filesystem, 0, sizeof(filesystem));
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT | SDMMC_HOST_FLAG_4BIT;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = 16; slot.cmd = 17;
    slot.d0 = 15; slot.d1 = 7; slot.d2 = 8; slot.d3 = 18;
    // Explicitly no CD/WP GPIO; do not invent a hardware removal signal.
    slot.cd = SDMMC_SLOT_NO_CD;
    slot.wp = SDMMC_SLOT_NO_WP;
    esp_err_t result = sdmmc_host_init();
    if (result != ESP_OK) goto fail;
    host_initialized = true;
    result = sdmmc_host_init_slot(host.slot, &slot);
    if (result != ESP_OK) goto fail;
    result = sdmmc_card_init(&host, &card);
    if (result != ESP_OK) goto fail;
    card_initialized = true;
    if (have_cid && memcmp(&last_cid, &card.cid, sizeof(last_cid)) != 0) {
        baseline_trusted = false;
        ESP_LOGW(TAG, "Card identity changed; old glass baseline forbidden");
    }
    last_cid = card.cid;
    have_cid = true;
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
    ff_sdmmc_set_disk_status_check(pdrv, true);
    drive[0] = (char)('0' + pdrv); drive[1] = ':'; drive[2] = '\0';
    filesystem_registered = true;
    FRESULT fr = f_mount(&filesystem, drive, 1);
    if (fr != FR_OK) {
        ESP_LOGW(TAG, "FatFs mount failed (%d); no format/repair", fr);
        result = fr == FR_NO_FILESYSTEM ? ESP_ERR_NOT_SUPPORTED : ESP_FAIL;
        goto fail;
    }
    return ESP_OK;
fail:
    ESP_LOGW(TAG, "SD unavailable (%s); next request may retry after 5s", esp_err_to_name(result));
    failed();
    return result;
}

static bool open_cache(void)
{
    if (mount_volume() != ESP_OK) return false;
    char path[] = "0:/" FRAME_CACHE_FILE_NAME;
    path[0] = drive[0];
    if (fat_frame_cache_open(&cache, path)) return true;
    ESP_LOGW(TAG, "Frame cache unavailable; full refresh, retry after 5s");
    failed();
    return false;
}

static bool finish(bool ok)
{
    // Do not call f_close (which calls f_sync) following any IO failure.
    if (ok) ok = fat_frame_cache_close(&cache);
    if (!ok) failed();
    else cache_unmount();
    return ok;
}

bool cache_read(uint8_t *frame, size_t size)
{
    if (!baseline_trusted || !open_cache()) return false;
    if (!baseline_trusted) { cache_unmount(); return false; }
    if (fat_frame_cache_read(&cache, frame, size)) return finish(true);
    // valid=0/CRC rejection is a cache miss, not evidence of faulty media.
    // Discard without f_close/f_sync, but do NOT back off: the same display
    // cycle must be able to invalidate and rebuild, even on a fast RTC boot.
    // A real read IO error is treated conservatively the same way; the next
    // operation must reinitialize/remount, never reuse this FIL or clusters.
    baseline_trusted = false;
    cache_unmount();
    return false;
}

bool cache_write(const uint8_t *frame, size_t size)
{
    bool written = open_cache() && finish(fat_frame_cache_write(&cache, frame, size));
    baseline_trusted = written;
    return written;
}

void cache_invalidate(void)
{
    bool was_trusted = baseline_trusted;
    baseline_trusted = false;
    int64_t now = esp_timer_get_time();
    // Menus redraw repeatedly. After losing trust, probe at most once per 5s;
    // successful frame commits restore trust and MUST invalidate immediately.
    if (!was_trusted && now < invalidate_after) return;
    invalidate_after = now + RETRY_US;
    if (open_cache() && !finish(fat_frame_cache_invalidate(&cache)))
        ESP_LOGW(TAG, "Unable to invalidate frame cache; old baseline forbidden");
}

void cache_diagnose(uint8_t *pattern, uint8_t *readback, size_t size,
                    cache_diagnostic_t *out)
{
    memset(out, 0, sizeof(*out));
    // The diagnostic page changes glass, including in gray mode. Never promote
    // its test pattern to the real cache or a trustworthy previous glass frame.
    baseline_trusted = false;
    out->mount = mount_volume();
    out->initialized = card_initialized;
    if (out->mount != ESP_OK) return;
    out->filesystem = filesystem.fs_type == FS_EXFAT ? "exFAT" :
                      filesystem.fs_type == FS_FAT32 ? "FAT32" : "FAT";
    if (!pattern || !readback || pattern == readback || size != FRAME_CACHE_FRAME_SIZE) {
        cache_unmount();
        return;
    }
    char path[96];
    uint32_t nonce_hi = esp_random(), nonce_lo = esp_random();
    bool unused = false;
    // Random nonce separates boots; the owner-local sequence separates calls.
    // Check metadata only, then FA_CREATE_NEW remains the authoritative guard
    // against races/collisions. Old failures and user files are never opened.
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        snprintf(path, sizeof(path), "%c:/epaper-cache-test-%08lx%08lx-%08lx.bin",
                 drive[0], (unsigned long)nonce_hi, (unsigned long)nonce_lo,
                 (unsigned long)++diagnostic_sequence);
        FRESULT fr = f_stat(path, NULL);
        if (fr == FR_NO_FILE) { unused = true; break; }
        if (fr != FR_OK) break;
    }
    // Failed IO may leave this call's partial file. A later call uses a new
    // name, never reopens/deletes a previous failure on a possibly changed card.
    out->created = unused && fat_frame_cache_create(&cache, path);
    if (!out->created) { failed(); return; }
    for (size_t i = 0; i < size; ++i)
        pattern[i] = (uint8_t)((i * 37u) ^ (i >> 8) ^ 0xa5u);
    out->written = fat_frame_cache_write(&cache, pattern, size);
    if (!out->written || !fat_frame_cache_close(&cache)) { failed(); return; }
    // Reopen to avoid claiming buffered readback as a disk/CRC test.
    if (!fat_frame_cache_open(&cache, path)) { failed(); return; }
    out->read_crc = fat_frame_cache_read(&cache, readback, size);
    out->compared = out->read_crc && memcmp(pattern, readback, size) == 0;
    if (!out->compared || !fat_frame_cache_close(&cache)) { failed(); return; }
    out->removed = f_unlink(path) == FR_OK; // Only our successfully created file
    if (!out->removed) failed();
    else cache_unmount();
    ESP_LOGW(TAG, "SD diagnostic: fs=%s write=%d read_crc=%d compare=%d cleanup=%d; NOT proof of minute partial refresh",
             out->filesystem, out->written, out->read_crc, out->compared, out->removed);
}
