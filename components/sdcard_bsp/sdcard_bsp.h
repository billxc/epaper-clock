#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
esp_err_t cache_mount(void);
bool cache_read(uint8_t *frame, size_t size);
bool cache_write(const uint8_t *frame, size_t size);
void cache_unmount(void);
// Drop only our frame cache when a refresh did not complete.
void cache_invalidate(void);
