#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

typedef struct {
    char ssid[33];
    char password[65];
} clock_wifi_config_t;

typedef struct {
    char password[17]; // Ephemeral HTTP password, never the router password.
    char ip[16];
} clock_management_info_t;

// Call once after NVS initialization. Wi-Fi stays off until requested.
// Serialize ALL operations (including stop/getters) across main/HTTP tasks,
// except set_poll_hook/cancel. HTTP must finish its current call before main
// tears down Wi-Fi or starts another operation. SNTP is owned by this module.
void network_init(void);
void network_get_config(clock_wifi_config_t *config);
bool network_configured(void);
// Versioned, single-item NVS save; RAM/daily-attempt reset only on success.
// Legacy keys are read without migration until an explicit successful save.
// Does not reconfigure a live station; stop before using changed credentials.
esp_err_t network_save_config(const clock_wifi_config_t *config);

// Called in the WAITING CALLER's task at <=100ms wait slices (not an ISR).
// Return true to latch cancellation, false to continue. NULL unregisters.
// Hook must be short, gate drawing on UI ownership (HTTP must not draw), and
// must NOT reenter network operations; it may call network_cancel(). It is
// also polled during mandatory stop cleanup, which cannot itself be aborted.
void network_set_poll_hook(bool (*hook)(void));
// Thread-safe cancellation request. Cancellable operations return
// ESP_ERR_INVALID_STATE. Does not stop Wi-Fi or join a running HTTP task.
void network_cancel(void);
// Only the serialized caller starts a NEW action by explicitly clearing this
// latch, after the old operation/task has finished. Services never clear it.
void network_clear_cancel(void);

// STA only, RAM Wi-Fi storage, no password generation or background retry.
// Reuse an existing connection; one new attempt if running but disconnected,
// without set_config on a running station. Association/DHCP wait <=15s.
// No saved SSID: ESP_ERR_NOT_FOUND. Caller owns radio lifetime/retry policy.
esp_err_t network_connect(void);
// Connect using saved Wi-Fi, then wait <=10s for a NEW SNTP response.
// Writes *epoch only on success. ESP SNTP adjusts system time, but this API
// NEVER writes PCF RTC, last_sync, last_attempt, or NVS. No SSID: NOT_FOUND.
// Limits concern waits; synchronous SDK calls and hook execution add latency.
esp_err_t network_query_time(time_t *epoch);
// Due once per local calendar day at/after CLOCK_SYNC_HOUR, provided RTC is valid.
// An unsuccessful attempt also consumes that day's scheduled attempt.
bool network_daily_sync(void);
// Query, validate RTC year, then write PCF RTC and persist existing sync state.
// Bypasses daily schedule; keeps the LAN/radio on for its caller.
esp_err_t network_sync_now(void);
// Retained admin entry point: connects and generates a NEW 16-hex password
// with RF active. info must be non-NULL; input password is not reused.
// Admin/OTA may overwrite it separately to reuse their own session password.
esp_err_t network_start_management(clock_management_info_t *info);
// Provisioning manager owns Wi-Fi start/connect between these calls (STA only).
esp_err_t network_prepare_provisioning(void);
void network_finish_provisioning(void);
// Mandatory cleanup even if cancelled, including manager-started Wi-Fi.
// Caller must first cancel AND wait for any HTTP network operation to finish.
void network_stop(void);
bool network_connected(void);
void network_station_ip(char *ip, unsigned size);
int64_t network_last_sync(void);
