#pragma once
#include <assert.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERROR_CHECK(expr) assert((expr) == ESP_OK)
const char *esp_err_to_name(esp_err_t err);
