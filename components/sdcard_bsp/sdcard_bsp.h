#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
// Caller serializes mount/unmount and I/O. Uses SDK FatFs directly, not VFS;
// mount failure disables the optional frame cache and must use full refresh.
esp_err_t cache_mount(void);
bool cache_read(uint8_t *frame, size_t size);
bool cache_write(const uint8_t *frame, size_t size);
void cache_unmount(void);
// Invalidate only the frame header; retain the preallocated FAT/exFAT cache file.
void cache_invalidate(void);
