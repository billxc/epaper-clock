#pragma once
#include "ff.h"
#include "esp_err.h"
#define FF_DRV_NOT_USED 0xff
esp_err_t ff_diskio_get_drive(BYTE *drive);
void ff_diskio_unregister(BYTE drive);
