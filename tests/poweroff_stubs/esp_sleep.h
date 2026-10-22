#pragma once
#include "../power_runtime_stubs/esp_sleep.h"
#define ESP_SLEEP_WAKEUP_ALL ((esp_sleep_wakeup_cause_t)99)
esp_err_t esp_sleep_disable_wakeup_source(esp_sleep_wakeup_cause_t source);
