#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
// UI owner only. Each operation owns a fresh, synchronous SD/FatFs session;
// no FIL/cluster object survives return. Swap cards only BETWEEN operations.
bool cache_read(uint8_t *frame, size_t size);
bool cache_write(const uint8_t *frame, size_t size);
void cache_unmount(void);
// Keep the recognized container; repeated untrusted-menu probes limited to 5s.
void cache_invalidate(void);

typedef struct {
    esp_err_t mount;
    const char *filesystem;
    bool initialized, created, written, read_crc, compared, removed;
} cache_diagnostic_t;
// Explicit diagnostic, also allowed in gray mode. Caller supplies two separate
// 48000-byte heap buffers, never task-stack frames. Does not modify RTC/WiFi.
void cache_diagnose(uint8_t *pattern, uint8_t *readback, size_t size,
                    cache_diagnostic_t *out);
