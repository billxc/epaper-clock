/* Handle-local invalidation regression with fault-injected FatFs API calls.
 * Complements (does not replace) the real SDK exFAT image integration tests.
 * Build with the same host config/SDK ff.h, but without ff.c/ffunicode.c:
 * cc -std=c11 -Wall -Wextra -Werror -include tests/fatfs_host_config.h \
 *   -I/path/to/sdk/fatfs/src -Icomponents/sdcard_bsp \
 *   tests/fat_frame_cache_state_test.c -o /tmp/fat-cache-state-test
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../components/sdcard_bsp/fat_frame_cache.c"

static uint8_t disk[FRAME_CACHE_FILE_SIZE], frame[FRAME_CACHE_FRAME_SIZE];
static unsigned header_writes, payload_writes, syncs;
static bool fail_write, short_write, fail_sync, fail_seek, fail_close, fail_open;
static bool fail_valid_header;

FRESULT f_open(FIL *file, const TCHAR *path, BYTE mode)
{
    (void)path; (void)mode;
    memset(file, 0, sizeof(*file));
    file->obj.objsize = sizeof(disk);
    return fail_open ? FR_DISK_ERR : FR_OK;
}
FRESULT f_read(FIL *file, void *out, UINT size, UINT *actual)
{
    assert(file->fptr + size <= sizeof(disk));
    memcpy(out, disk + file->fptr, size);
    file->fptr += size;
    *actual = size;
    return FR_OK;
}
FRESULT f_write(FIL *file, const void *data, UINT size, UINT *actual)
{
    if (file->fptr == 0) ++header_writes;
    else ++payload_writes;
    *actual = 0;
    if (fail_write || (fail_valid_header && file->fptr == 0 &&
                       get32((const uint8_t *)data + 16) == 1))
        return FR_DISK_ERR;
    if (short_write) return FR_OK;
    assert(file->fptr + size <= sizeof(disk));
    memcpy(disk + file->fptr, data, size);
    file->fptr += size;
    *actual = size;
    return FR_OK;
}
FRESULT f_lseek(FIL *file, FSIZE_t offset)
{
    if (fail_seek) return FR_DISK_ERR;
    assert(offset <= sizeof(disk));
    file->fptr = offset;
    return FR_OK;
}
FRESULT f_sync(FIL *file)
{
    (void)file;
    ++syncs;
    return fail_sync ? FR_DISK_ERR : FR_OK;
}
FRESULT f_close(FIL *file)
{
    (void)file;
    return fail_close ? FR_DISK_ERR : FR_OK;
}

static void fresh(fat_frame_cache_t *cache)
{
    memset(cache, 0, sizeof(*cache));
    memset(disk, 0, sizeof(disk));
    make_header(cache, false, 0);
    memcpy(disk, cache->io, FRAME_CACHE_HEADER_SIZE);
    header_writes = payload_writes = syncs = 0;
    fail_write = short_write = fail_sync = fail_seek = fail_close = fail_open = false;
    fail_valid_header = false;
    assert(fat_frame_cache_open(cache, "0:/epaper-cache.bin"));
    assert(!cache->invalid_synced); // Never inferred from an on-disk invalid bit.
}

int main(void)
{
    fat_frame_cache_t cache;
    memset(frame, 0x5a, sizeof(frame));
    fresh(&cache);
    assert(fat_frame_cache_invalidate(&cache));
    assert(cache.invalid_synced && header_writes == 1 && syncs == 1);
    assert(fat_frame_cache_invalidate(&cache));
    assert(header_writes == 1 && syncs == 1); // No duplicate header or sync.
    assert(f_lseek(&cache.file, 9000) == FR_OK);
    assert(fat_frame_cache_write(&cache, frame, sizeof(frame)));
    assert(header_writes == 2 && syncs == 2 && !cache.invalid_synced);
    assert(!memcmp(disk + FRAME_CACHE_HEADER_SIZE, frame, sizeof(frame)));
    assert(fat_frame_cache_invalidate(&cache));
    assert(header_writes == 3 && syncs == 3);
    fat_frame_cache_close(&cache);
    assert(!cache.opened && !cache.invalid_synced);
    assert(fat_frame_cache_open(&cache, "0:/epaper-cache.bin"));
    assert(!cache.invalid_synced && fat_frame_cache_invalidate(&cache));
    assert(header_writes == 4 && syncs == 4); // Reopen requires fresh sync.

    // Both hard header errors and short writes must block payload completely.
    for (int short_case = 0; short_case < 2; ++short_case) {
        fresh(&cache);
        fail_write = !short_case;
        short_write = short_case;
        assert(!fat_frame_cache_invalidate(&cache) && !cache.invalid_synced);
        assert(!fat_frame_cache_write(&cache, frame, sizeof(frame)));
        assert(!cache.invalid_synced && header_writes == 2 && !payload_writes && !syncs);
        fail_write = short_write = false;
        assert(fat_frame_cache_write(&cache, frame, sizeof(frame)));
        assert(header_writes == 4 && syncs == 2);
    }
    fresh(&cache);
    fail_sync = true;
    assert(!fat_frame_cache_invalidate(&cache) && !cache.invalid_synced);
    assert(!fat_frame_cache_write(&cache, frame, sizeof(frame)));
    assert(header_writes == 2 && syncs == 2 && !payload_writes);
    fail_sync = false;
    assert(fat_frame_cache_write(&cache, frame, sizeof(frame)));
    assert(header_writes == 4 && syncs == 4);

    fresh(&cache);
    fail_seek = true;
    assert(!fat_frame_cache_invalidate(&cache) && !cache.invalid_synced);
    assert(!header_writes && !syncs);
    fail_seek = false;
    assert(fat_frame_cache_invalidate(&cache));
    fail_write = true; // Payload failure must revoke the optimization as well.
    assert(!fat_frame_cache_write(&cache, frame, sizeof(frame)) && !cache.invalid_synced);
    fail_write = false;
    assert(fat_frame_cache_invalidate(&cache) && header_writes == 2);
    fail_close = true;
    fat_frame_cache_close(&cache);
    assert(!cache.opened && !cache.invalid_synced);
    cache.invalid_synced = true; // Even failed open must discard stale state.
    fail_open = true;
    assert(!fat_frame_cache_open(&cache, "0:/epaper-cache.bin") && !cache.invalid_synced);

    // A failed valid-header write also requires a new invalid header + sync.
    fresh(&cache);
    assert(fat_frame_cache_invalidate(&cache));
    fail_valid_header = true;
    assert(!fat_frame_cache_write(&cache, frame, sizeof(frame)) && !cache.invalid_synced);
    assert(header_writes == 2 && syncs == 1);
    fail_valid_header = false;
    assert(fat_frame_cache_invalidate(&cache));
    assert(header_writes == 3 && syncs == 2);

    // A failed final valid-header sync cannot masquerade as durable invalidity.
    fresh(&cache);
    assert(fat_frame_cache_invalidate(&cache));
    fail_sync = true;
    assert(!fat_frame_cache_write(&cache, frame, sizeof(frame)) && !cache.invalid_synced);
    assert(get32(disk + 16) == 1); // Valid header reached the file before failure.
    fail_sync = false;
    assert(fat_frame_cache_invalidate(&cache));
    assert(get32(disk + 16) == 0 && header_writes == 3 && syncs == 3);
    puts("FatFs handle-local invalidation/failure tests passed");
    return 0;
}
