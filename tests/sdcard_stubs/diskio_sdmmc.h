#pragma once
#include "ff.h"
#include "sdmmc_cmd.h"
#include <stdbool.h>
void ff_diskio_register_sdmmc(BYTE drive, sdmmc_card_t *card);
void ff_sdmmc_set_disk_status_check(BYTE drive, bool enable);
