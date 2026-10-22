#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    POWER_WAKE_MANUAL,
    POWER_WAKE_SCHEDULED,
    POWER_WAKE_CENTER,
} power_wake_t;

// Owner task, before app_ui_init: restore GPIO5 from RTC IO after deep wake.
// Only a cold boot may use the external RTC alarm as its wake classification.
power_wake_t power_runtime_wake(bool rtc_alarm);
// Owner task only, after axp_init and on each app_poll. HTTP never touches I2C.
void power_runtime_apply_settings(void);
// Seconds-resolution external RTC alignment; invalid RTC retries in 60s.
uint64_t power_runtime_minute_us(bool valid, int second);
// Clock owner only, AFTER network_stop/cache_unmount and eligibility recheck.
// Returns without sleeping if center is held or wake setup fails. Never falls
// back to PMU shutdown when deep sleep was requested.
void power_runtime_sleep(void);
