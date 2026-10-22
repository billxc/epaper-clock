#pragma once
#include "esp_err.h"

typedef enum { POWER_SLEEP_PMU = 0, POWER_SLEEP_DEEP = 1 } power_sleep_mode_t;
typedef struct {
    power_sleep_mode_t sleep_mode;
    unsigned pwr_on_ms;
} power_settings_t;

// Initialize on the owner task before starting HTTP. Does not touch hardware.
void power_settings_init(void);
// Coherent, thread-safe saved snapshot; not a claim of hardware application.
power_settings_t power_settings_get(void);
esp_err_t power_settings_save(power_settings_t settings);
const char *power_settings_warning(void);
