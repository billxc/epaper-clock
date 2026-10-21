#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EXT4_CACHE_FILE_NAME "epaper-cache.bin"
#define EXT4_CACHE_FILE_SIZE 65536u
#define EXT4_CACHE_FRAME_SIZE 48000u
#define EXT4_CACHE_SECTOR_SIZE 512u
#define EXT4_CACHE_IO_SECTORS 8u

/* Callbacks transfer count consecutive 512-byte sectors (1..IO_SECTORS).
 * All transfers use the cache's aligned scratch, never the caller's frame. */
typedef bool (*ext4_cache_read_sectors_t)(void *context, uint64_t lba,
                                          size_t count, void *data);
typedef bool (*ext4_cache_write_sectors_t)(void *context, uint64_t lba,
                                           size_t count, const void *data);

typedef struct {
    ext4_cache_read_sectors_t read;
    ext4_cache_write_sectors_t write;
    void *context;
    uint64_t sectors;
    uint64_t file_lba;
    bool opened;
    _Alignas(4) uint8_t sector[EXT4_CACHE_IO_SECTORS * EXT4_CACHE_SECTOR_SIZE];
} ext4_cache_t;

/* Read-only discovery: clean 4KiB ext4, root-level fixed file, initialized
 * single extent, 64KiB size. Supports a whole-device filesystem or MBR Linux
 * partition. No filesystem metadata is written; unsupported layouts fail. */
bool ext4_cache_open(ext4_cache_t *cache, ext4_cache_read_sectors_t read,
                     ext4_cache_write_sectors_t write, void *context,
                     uint64_t sectors);
bool ext4_cache_read(ext4_cache_t *cache, uint8_t *frame, size_t size);
bool ext4_cache_write(ext4_cache_t *cache, const uint8_t *frame, size_t size);
bool ext4_cache_invalidate(ext4_cache_t *cache);

/* File layout: 512-byte header followed by 48,000 frame bytes; remaining
 * bytes reserved. Header: magic "EPDCACHE" (8), LE32 version=1, frame_size,
 * valid (0/1), frame_crc32, header_crc32 over bytes [0,24); rest zero.
 * CRC32 is IEEE/zlib. Payload is written before its committed header. */
