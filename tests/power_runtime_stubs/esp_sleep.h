#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef enum { ESP_SLEEP_WAKEUP_UNDEFINED, ESP_SLEEP_WAKEUP_TIMER,
               ESP_SLEEP_WAKEUP_EXT1, ESP_SLEEP_WAKEUP_OTHER } esp_sleep_wakeup_cause_t;
typedef enum { ESP_EXT1_WAKEUP_ANY_LOW, ESP_EXT1_WAKEUP_ANY_HIGH } esp_sleep_ext1_wakeup_mode_t;
esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause(void);
uint64_t esp_sleep_get_ext1_wakeup_status(void);
esp_err_t esp_sleep_disable_ext1_wakeup_io(uint64_t mask);
esp_err_t esp_sleep_enable_ext1_wakeup_io(uint64_t mask, esp_sleep_ext1_wakeup_mode_t mode);
esp_err_t esp_sleep_enable_timer_wakeup(uint64_t us);
void esp_deep_sleep_start(void) __attribute__((noreturn));
