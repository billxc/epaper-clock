#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    char ssid[33];
    char password[65];
} clock_wifi_config_t;

typedef struct {
    char ssid[33];
    char password[17];
    char ip[16];
} clock_ap_info_t;

// Call once after NVS initialization. Wi-Fi stays off until requested.
void network_init(void);
void network_get_config(clock_wifi_config_t *config);
esp_err_t network_save_config(const clock_wifi_config_t *config);
// Due once per local calendar day at/after CLOCK_SYNC_HOUR, provided RTC is valid.
// An unsuccessful attempt also consumes that day's scheduled attempt.
bool network_daily_sync(void);
// User-requested synchronization; bypasses the daily schedule.
esp_err_t network_sync_now(void);
esp_err_t network_start_management(clock_ap_info_t *info);
void network_stop(void);
bool network_connected(void);
void network_station_ip(char *ip, unsigned size);
int64_t network_last_sync(void);
