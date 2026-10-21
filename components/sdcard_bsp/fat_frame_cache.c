/* EPDCACHE v1 container on an already-mounted FatFs volume. */
#include "fat_frame_cache.h"

#include <string.h>

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

/* Reflected IEEE CRC-32; callers supply the initial/final complement. */
static uint32_t crc_ieee(uint32_t crc, const uint8_t *p, size_t size)
{
    static const uint32_t table[16] = {
        0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu,
        0x76dc4190u, 0x6b6b51f4u, 0x4db26158u, 0x5005713cu,
        0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
        0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu,
    };
    while (size-- != 0u) {
        crc ^= *p++;
        crc = (crc >> 4) ^ table[crc & 15u];
        crc = (crc >> 4) ^ table[crc & 15u];
    }
    return crc;
}

static bool read_bytes(fat_frame_cache_t *cache, UINT count)
{
    UINT actual = 0;
    return f_read(&cache->file, cache->io, count, &actual) == FR_OK &&
           actual == count;
}

static bool write_bytes(fat_frame_cache_t *cache, UINT count)
{
    UINT actual = 0;
    return f_write(&cache->file, cache->io, count, &actual) == FR_OK &&
           actual == count;
}

static void make_header(fat_frame_cache_t *cache, bool valid, uint32_t frame_crc)
{
    uint8_t *h = cache->io;
    memset(h, 0, FRAME_CACHE_HEADER_SIZE);
    memcpy(h, "EPDCACHE", 8u);
    put32(h + 8, 1u);
    put32(h + 12, FRAME_CACHE_FRAME_SIZE);
    put32(h + 16, valid ? 1u : 0u);
    put32(h + 20, frame_crc);
    put32(h + 24, ~crc_ieee(~(uint32_t)0, h, 24u));
}

/* Identity is deliberately stricter than magic alone, but permits damaged
 * valid/headerCRC fields so a recognized interrupted cache can be rewritten. */
static bool header_identity(const uint8_t *h)
{
    if (memcmp(h, "EPDCACHE", 8u) != 0 || get32(h + 8) != 1u ||
        get32(h + 12) != FRAME_CACHE_FRAME_SIZE)
        return false;
    for (size_t i = 28u; i < FRAME_CACHE_HEADER_SIZE; ++i)
        if (h[i] != 0u)
            return false;
    return true;
}

static bool create_container(fat_frame_cache_t *cache)
{
    /* Materialize every byte: seeking to EOF alone does not initialize data. */
    memset(cache->io, 0, sizeof(cache->io));
    for (size_t offset = 0; offset < FRAME_CACHE_FILE_SIZE;
         offset += FRAME_CACHE_IO_SIZE)
        if (!write_bytes(cache, FRAME_CACHE_IO_SIZE))
            return false;
    make_header(cache, false, 0u);
    return f_lseek(&cache->file, 0u) == FR_OK &&
           write_bytes(cache, FRAME_CACHE_HEADER_SIZE) &&
           f_sync(&cache->file) == FR_OK &&
           f_size(&cache->file) == FRAME_CACHE_FILE_SIZE;
}

bool fat_frame_cache_open(fat_frame_cache_t *cache, const char *path)
{
    FRESULT result;
    bool ready;
    if (cache == NULL)
        return false;
    cache->invalid_synced = false;
    if (path == NULL)
        return false;
    /* The caller supplies a fresh/closed context; opened is set only on success. */
    cache->opened = false;
    result = f_open(&cache->file, path, FA_READ | FA_WRITE);
    if (result == FR_NO_FILE) {
        result = f_open(&cache->file, path, FA_READ | FA_WRITE | FA_CREATE_NEW);
        if (result != FR_OK)
            return false;
        ready = create_container(cache);
    } else {
        if (result != FR_OK)
            return false;
        ready = f_size(&cache->file) == FRAME_CACHE_FILE_SIZE &&
                read_bytes(cache, FRAME_CACHE_HEADER_SIZE) &&
                header_identity(cache->io);
    }
    if (!ready && f_close(&cache->file) != FR_OK)
        return false;
    cache->opened = ready;
    return ready;
}

bool fat_frame_cache_read(fat_frame_cache_t *cache, uint8_t *frame, size_t size)
{
    uint32_t expected, crc = ~(uint32_t)0;
    size_t remaining = FRAME_CACHE_FRAME_SIZE;
    if (cache == NULL || !cache->opened || frame == NULL ||
        size != FRAME_CACHE_FRAME_SIZE ||
        f_lseek(&cache->file, 0u) != FR_OK ||
        !read_bytes(cache, FRAME_CACHE_HEADER_SIZE) ||
        !header_identity(cache->io) || get32(cache->io + 16) != 1u ||
        get32(cache->io + 24) != ~crc_ieee(~(uint32_t)0, cache->io, 24u))
        return false;
    expected = get32(cache->io + 20);
    while (remaining != 0u) {
        UINT count = remaining < FRAME_CACHE_IO_SIZE ?
                     (UINT)remaining : FRAME_CACHE_IO_SIZE;
        if (!read_bytes(cache, count))
            return false;
        crc = crc_ieee(crc, cache->io, count);
        memcpy(frame, cache->io, count);
        frame += count;
        remaining -= count;
    }
    /* On any failure the caller must discard the possibly partial output. */
    return ~crc == expected;
}

bool fat_frame_cache_invalidate(fat_frame_cache_t *cache)
{
    if (cache == NULL || !cache->opened)
        return false;
    if (cache->invalid_synced)
        return true;
    make_header(cache, false, 0u);
    cache->invalid_synced = f_lseek(&cache->file, 0u) == FR_OK &&
           write_bytes(cache, FRAME_CACHE_HEADER_SIZE) &&
           f_sync(&cache->file) == FR_OK;
    return cache->invalid_synced;
}

bool fat_frame_cache_write(fat_frame_cache_t *cache, const uint8_t *frame,
                           size_t size)
{
    uint32_t crc = ~(uint32_t)0;
    size_t remaining = FRAME_CACHE_FRAME_SIZE;
    if (cache == NULL || !cache->opened || frame == NULL ||
        size != FRAME_CACHE_FRAME_SIZE || !fat_frame_cache_invalidate(cache))
        return false;
    /* An already-synced invalidation does not move the file pointer. Always
     * position explicitly, including retries after partial payload writes. */
    if (f_lseek(&cache->file, FRAME_CACHE_HEADER_SIZE) != FR_OK) {
        cache->invalid_synced = false;
        return false;
    }
    while (remaining != 0u) {
        UINT count = remaining < FRAME_CACHE_IO_SIZE ?
                     (UINT)remaining : FRAME_CACHE_IO_SIZE;
        memcpy(cache->io, frame, count);
        crc = crc_ieee(crc, cache->io, count);
        if (!write_bytes(cache, count)) {
            cache->invalid_synced = false;
            return false;
        }
        frame += count;
        remaining -= count;
    }
    /* The final 2944 bytes end partway through a sector. f_lseek(0) alone
     * need not flush it: FatFs f_write at the new sector boundary writes back
     * the dirty file buffer BEFORE writing the header (FF_FS_TINY likewise
     * syncs the shared window). Final f_sync flushes metadata and CTRL_SYNC.
     * No per-chunk sync or writes beyond the payload are needed. */
    /* Once a valid header may be buffered/written, a later invalidate must
     * write and sync again, even when this commit fails. Clear BEFORE IO. */
    cache->invalid_synced = false;
    if (f_lseek(&cache->file, 0u) != FR_OK)
        return false;
    make_header(cache, true, ~crc);
    return write_bytes(cache, FRAME_CACHE_HEADER_SIZE) &&
           f_sync(&cache->file) == FR_OK;
}

void fat_frame_cache_close(fat_frame_cache_t *cache)
{
    if (cache == NULL)
        return;
    cache->invalid_synced = false;
    if (!cache->opened)
        return;
    FRESULT result = f_close(&cache->file);
    cache->opened = false;
    /* This void interface cannot report close failure; never retry writes or
     * repair/delete the container on a failing volume. */
    if (result != FR_OK)
        return;
}
