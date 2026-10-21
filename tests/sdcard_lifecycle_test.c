/* Real SDK FatFs + regular image, fake SDMMC transport/lifecycle only.
 * This does NOT emulate hardware detection, exFAT hard-removal safety or EPD.
 * Run through sdcard_lifecycle_image_test.py; never pass a block device.
 * Build like fat_frame_cache_test.c, adding -Itests/sdcard_stubs and replacing
 * that harness with this file plus components/sdcard_bsp/sdcard_bsp.c.
 */
#include <assert.h>
#include <unistd.h>
static ssize_t injected_pwrite(int fd, const void *data, size_t size, off_t offset);
#define main original_image_harness_main
#define pwrite injected_pwrite
#define ff_memalloc original_memalloc
#define ff_memfree original_memfree
#include "fat_frame_cache_test.c"
#undef main
#undef pwrite
#undef ff_memalloc
#undef ff_memfree
#include "sdcard_bsp.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "driver/sdmmc_host.h"

static int64_t clock_us = 1;
static bool present, host_live, disk_live;
static uint32_t cid = 1;
static unsigned host_inits, allocations, frees, fail_write_at, write_calls;
static bool fail_diagnostic_payload;
static uint8_t pattern[FRAME_CACHE_FRAME_SIZE], readback[FRAME_CACHE_FRAME_SIZE];

void *ff_memalloc(UINT size) { ++allocations; return malloc(size); }
void ff_memfree(void *p) { if (p) ++frees; free(p); }
static ssize_t injected_pwrite(int fd, const void *data, size_t size, off_t offset)
{
    if (++write_calls == fail_write_at ||
        (fail_diagnostic_payload && size >= 32 && !memcmp(data, pattern, 32))) {
        errno = EIO;
        return -1;
    }
    return pwrite(fd, data, size, offset);
}
int64_t esp_timer_get_time(void) { return clock_us; }
uint32_t esp_random(void) { return 0x12345678; } // Deterministic collision fixture
const char *esp_err_to_name(esp_err_t e) { return e ? "error" : "ok"; }
esp_err_t sdmmc_host_init(void)
{
    assert(!host_live && !disk_live);
    host_live = true;
    ++host_inits;
    return ESP_OK;
}
esp_err_t sdmmc_host_init_slot(int slot, const sdmmc_slot_config_t *config)
{
    assert(host_live && slot == 1 && config->cd == -1 && config->wp == -1);
    return ESP_OK;
}
esp_err_t sdmmc_card_init(const sdmmc_host_t *host, sdmmc_card_t *card)
{
    (void)host;
    if (!present) return ESP_FAIL;
    card->cid.serial = cid;
    card->csd.sector_size = 512;
    return ESP_OK;
}
esp_err_t sdmmc_host_deinit(void)
{
    assert(host_live && !disk_live);
    host_live = false;
    return ESP_OK;
}
esp_err_t ff_diskio_get_drive(BYTE *drive) { *drive = 0; return ESP_OK; }
void ff_diskio_register_sdmmc(BYTE drive, sdmmc_card_t *card)
{
    assert(drive == 0 && card && host_live && !disk_live);
    disk_live = true;
}
void ff_sdmmc_set_disk_status_check(BYTE drive, bool enabled)
{
    assert(drive == 0 && enabled && disk_live);
}
void ff_diskio_unregister(BYTE drive) { assert(drive == 0 && disk_live); disk_live = false; }

static void detached(void)
{
    assert(!host_live && !disk_live);
    assert(allocations == frees); // Includes failed mounts and FIL abandonment
    uint64_t writes = image.writes;
    cache_unmount();
    assert(image.writes == writes); // Recovery never flushes an old file/window
}
static void later(void) { clock_us += 6000000; image.failed = false; }

int main(int argc, char **argv)
{
    assert(argc == 3); // IMAGE (private regular image), case
    image.fd = regular_open(argv[1], O_RDWR, false);
    assert(image.fd >= 0);
    struct stat st;
    assert(fstat(image.fd, &st) == 0 && st.st_size > 0 && st.st_size % SECTOR_SIZE == 0);
    image.sectors = (uint64_t)st.st_size / SECTOR_SIZE;
    if (!strcmp(argv[2], "failed-mount")) {
        present = true; // Deliberately unformatted regular image, not a device
        for (int i = 0; i < 10; ++i) {
            cache_diagnostic_t result;
            cache_diagnose(pattern, readback, sizeof(pattern), &result);
            assert(result.mount != ESP_OK && !result.created);
            detached(); later();
        }
    } else {
        // A fresh valid=0 container and both kinds of CRC miss must rebuild in
        // this SAME RTC cycle: deliberately never advance the simulated clock.
        present = true;
        memset(pattern, 0x5a, sizeof(pattern));
        int64_t same_tick = clock_us;
        assert(!cache_read(readback, sizeof(readback))); detached();
        cache_invalidate(); detached();
        assert(cache_write(pattern, sizeof(pattern))); detached();
        assert(clock_us == same_tick);
        const uint64_t corrupt_offsets[] = {24, FRAME_CACHE_HEADER_SIZE};
        for (size_t i = 0; i < sizeof(corrupt_offsets) / sizeof(corrupt_offsets[0]); ++i) {
            FATFS corrupt_fs = {0};
            assert(f_mount(&corrupt_fs, "0:", 1) == FR_OK);
            assert(overwrite(corrupt_offsets[i], 0xff) == 0);
            assert(f_mount(NULL, "0:", 0) == FR_OK);
            uint64_t before_read = image.writes;
            assert(!cache_read(readback, sizeof(readback))); detached();
            assert(image.writes == before_read); // Read miss never close/flushes
            cache_invalidate(); detached();
            assert(cache_write(pattern, sizeof(pattern))); detached();
            assert(cache_read(readback, sizeof(readback)));
            assert(!memcmp(pattern, readback, sizeof(pattern))); detached();
            assert(clock_us == same_tick);
        }
        puts("PASS no-time-advance valid0/headerCRC/frameCRC miss -> invalidate -> write/read recovery");

        // No card -> bounded retries -> insertion recovers in the same process.
        present = false;
        assert(!cache_write(pattern, sizeof(pattern)));
        detached();
        unsigned attempts = host_inits;
        present = true;
        cache_invalidate();
        assert(!cache_write(pattern, sizeof(pattern)) && host_inits == attempts);
        later();
        memset(pattern, 0x5a, sizeof(pattern));
        assert(cache_write(pattern, sizeof(pattern))); detached();
        assert(cache_read(readback, sizeof(readback)) && !memcmp(pattern, readback, sizeof(pattern)));
        detached();

        // Known successful invalidation must not init/write at every redraw.
        cache_invalidate(); detached();
        attempts = host_inits;
        uint64_t writes = image.writes;
        cache_invalidate(); cache_invalidate();
        assert(host_inits == attempts && image.writes == writes);
        assert(!cache_read(readback, sizeof(readback)));
        later();
        assert(cache_write(pattern, sizeof(pattern))); detached();

        // A replacement card's valid CRC is NOT a matching glass baseline.
        ++cid;
        assert(!cache_read(readback, sizeof(readback))); detached();
        assert(cache_write(pattern, sizeof(pattern))); detached();
        assert(cache_read(readback, sizeof(readback))); detached();

        // Write failure discards buffers. A later card/session cannot receive
        // an old FIL close/flush, and successful write restores read eligibility.
        fail_write_at = write_calls + 2;
        assert(!cache_write(pattern, sizeof(pattern))); detached();
        fail_write_at = 0; ++cid; later();
        assert(cache_write(pattern, sizeof(pattern))); detached();
        assert(cache_read(readback, sizeof(readback))); detached();
        present = false;
        cache_invalidate(); detached();
        present = true; later();
        assert(cache_write(pattern, sizeof(pattern))); detached();

        // Diagnostic uses its own create-new path, closes/reopens and verifies
        // a full 48000-byte CRC+comparison. Never trusts the formal cache after.
        cache_diagnostic_t result;
        bool diagnostic_failure = !strcmp(argv[2], "diagnostic-io-failure");
        fail_diagnostic_payload = diagnostic_failure;
        cache_diagnose(pattern, readback, sizeof(pattern), &result);
        fail_diagnostic_payload = false;
        if (diagnostic_failure) {
            assert(result.mount == ESP_OK && result.created && !result.written && !result.removed);
            detached(); later();
            // Fresh unique filename succeeds; the failed call's file survives.
            cache_diagnose(pattern, readback, sizeof(pattern), &result);
            assert(result.mount == ESP_OK && result.created && result.written &&
                   result.read_crc && result.compared && result.removed);
        } else if (!strcmp(argv[2], "collision")) {
            // Metadata collision skips the user's file, without opening it.
            assert(result.mount == ESP_OK && result.created && result.written &&
                   result.read_crc && result.compared && result.removed);
        } else {
            assert(result.mount == ESP_OK && result.created && result.written &&
                   result.read_crc && result.compared && result.removed);
        }
        detached();
        assert(!cache_read(readback, sizeof(readback)));
        // The diagnostic never writes its pattern into the formal container.
        // Inspect directly, independent of the wrapper's now-untrusted gate.
        FATFS check_fs = {0};
        fat_frame_cache_t check_cache = {0};
        assert(f_mount(&check_fs, "0:", 1) == FR_OK);
        assert(fat_frame_cache_open(&check_cache, CACHE_PATH));
        assert(fat_frame_cache_read(&check_cache, readback, sizeof(readback)));
        for (size_t i = 0; i < sizeof(readback); ++i) assert(readback[i] == 0x5a);
        assert(fat_frame_cache_close(&check_cache));
        assert(f_mount(NULL, "0:", 0) == FR_OK);
        detached();
        later();
        assert(cache_write(pattern, sizeof(pattern))); detached();
    }
    assert(close(image.fd) == 0);
    printf("PASS lifecycle %s: real FatFs, allocations=%u frees=%u; not hardware/power-loss validation\n",
           argv[2], allocations, frees);
    return 0;
}
