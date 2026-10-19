#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// Main task: call after NVS/I2C/PMU/RTC read/EPD port initialization, BEFORE
// any minute-alarm poweroff. healthy must describe mandatory local hardware,
// not Wi-Fi, NTP or optional SD. A pending unhealthy image rolls back/reboots.
// False means confirmation/rollback failed: supervisor must not power off.
bool ota_boot_check(bool healthy);
bool ota_resume_management(void);
// Same Basic password across new-image AND rollback boots. Never log this.
bool ota_get_password_override(char out[17]);
// On leaving resumed management, remove the persisted credential/transaction.
// Not called on a reboot requested by OTA. No writes on ordinary clock boots.
void ota_management_finished(void);

typedef struct {
    char state[24]; // idle/uploading/verifying/rebooting/success/rollback/error
    char message[128];
    char expected_version[33];
    size_t received;
    size_t total;
} ota_status_t;
void ota_get_status(ota_status_t *out);
bool ota_busy(void); // Includes verification and pending reboot.
bool ota_reboot_requested(void);

// HTTP task only; serialized by the HTTP server. Constant-size streaming RAM.
// begin validates length against the inactive application partition.
esp_err_t ota_begin(size_t length, const char *password);
esp_err_t ota_write(const void *data, size_t length);
esp_err_t ota_finish(void);
void ota_abort(const char *reason);
// Call AFTER the success response send attempt; main delays restart, never HTTP.
void ota_response_sent(void);
