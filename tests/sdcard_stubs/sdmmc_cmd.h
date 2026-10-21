#pragma once
#include "esp_err.h"
#include <stdint.h>
typedef struct { uint32_t serial; } sdmmc_cid_t;
typedef struct { sdmmc_cid_t cid; struct { int sector_size; } csd; } sdmmc_card_t;
typedef struct { int flags, max_freq_khz, slot; } sdmmc_host_t;
esp_err_t sdmmc_card_init(const sdmmc_host_t *host, sdmmc_card_t *card);
