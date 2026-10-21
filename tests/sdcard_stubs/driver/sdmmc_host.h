#pragma once
#include "sdmmc_cmd.h"
#define SDMMC_HOST_DEFAULT() ((sdmmc_host_t){.slot = 1})
#define SDMMC_HOST_FLAG_1BIT 1
#define SDMMC_HOST_FLAG_4BIT 4
#define SDMMC_FREQ_HIGHSPEED 40000
#define SDMMC_SLOT_NO_CD -1
#define SDMMC_SLOT_NO_WP -1
typedef struct { int width, clk, cmd, d0, d1, d2, d3, cd, wp; } sdmmc_slot_config_t;
#define SDMMC_SLOT_CONFIG_DEFAULT() ((sdmmc_slot_config_t){.cd = -1, .wp = -1})
esp_err_t sdmmc_host_init(void);
esp_err_t sdmmc_host_init_slot(int slot, const sdmmc_slot_config_t *config);
esp_err_t sdmmc_host_deinit(void);
