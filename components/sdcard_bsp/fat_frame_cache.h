#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ff.h"

#define FRAME_CACHE_FILE_NAME "epaper-cache.bin"
#define FRAME_CACHE_FILE_SIZE 65536u
#define FRAME_CACHE_FRAME_SIZE 48000u
#define FRAME_CACHE_HEADER_SIZE 512u
#define FRAME_CACHE_IO_SIZE 4096u

typedef struct {
    FIL file;
    bool opened;
    _Alignas(4) uint8_t io[FRAME_CACHE_IO_SIZE];
} fat_frame_cache_t;

/* Use an already-mounted FatFs volume. Create only the named cache if absent;
 * never truncate an existing unrecognized file, format, or repair a volume. */
bool fat_frame_cache_open(fat_frame_cache_t *cache, const char *path);
bool fat_frame_cache_read(fat_frame_cache_t *cache, uint8_t *frame, size_t size);
bool fat_frame_cache_write(fat_frame_cache_t *cache, const uint8_t *frame, size_t size);
bool fat_frame_cache_invalidate(fat_frame_cache_t *cache);
void fat_frame_cache_close(fat_frame_cache_t *cache);

/* 64KiB container: header512, payload48000, rest reserved. Header has magic
 * EPDCACHE[8], LE32 version=1, frame_size, valid, IEEE frameCRC, headerCRC over
 * bytes[0,24). Header[28,512) zero. Commit invalid -> payload -> valid + sync. */
