#pragma once

#include <stdbool.h>
#include "app_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    app_action_t next;
    bool wifi_saved; // At least one verified candidate committed THIS invocation.
    bool time_set;   // At least one manual RTC write THIS invocation.
} provision_result_t;

/**
 * Blocking Security1 + fresh six-digit PoP BLE Wi-Fi/manual RTC service.
 * Call ONLY on the UI-owning application task, after NVS/network/RTC/UI init.
 * Caller sets app_ui_set_mode(APP_INITIALIZE or APP_BLE) before entry, and
 * performs a full clock refresh after return. provision_render() remains the
 * PoP renderer; frames go through app_ui_present(frame, true), never raw EPD.
 * app_poll() is pumped throughout service and initial-mode retry waiting.
 * A menu action interrupts after cleanup and is returned in next, unchanged.
 *
 * initial=true: 295s inactivity radio window; failure/timeout stops BLE then
 * waits for middle-key select to retry (fresh PoP), or a menu action. Existing
 * SSID/RTC do not complete forced initialization. A new Wi-Fi save, or a new
 * manual-time event when Wi-Fi is already configured, returns APP_CLOCK after
 * a bounded ACK grace (2s after generating status, at
 * most 15s after completion). Parent decides NTP/valid-RTC clock entry policy.
 * initial=false: remain in management after save/set_time, allow repeated Wi-Fi
 * submissions, return APP_CLOCK after 300s inactivity. Authenticated prepare,
 * credentials, set_time and physical activity renew the window; status polling
 * does not. Error/unexpected SDK END cleans up and returns APP_CLOCK.
 * Flags are cumulative, not a claim that a BLE disconnect delivered an ACK.
 *
 * Integration contracts:
 * - network_prepare_provisioning(): init STA, do not start/connect; no competing
 *   reconnect handler. network_finish_provisioning(): unconditional stop/deinit,
 *   drain events, even after partially failed preparation.
 * - network_save_config(): only called on this task after CRED_SUCCESS (GOT_IP).
 *   Must preserve old clock_net on failure. status.wifi_saved only becomes true
 *   after this call succeeds. Never save in SDK callbacks or at cleanup.
 * - rtc_set(epoch)/rtc_read(): parent supplies a shared RTC mutex. Endpoint task
 *   may call them; RTC stores device-local calendar. Device TZ must already be
 *   configured; accepted UTC epoch is 946684800..4102444799, with local year also
 *   2000..2099. rtc_set is currently void/fatal on I2C error. UI notification is
 *   atomic and consumed on the application task, never rendered by callbacks.
 * - No concurrent RF/ADC at entry (hardware entropy for PoP); Wi-Fi/BT off.
 *   Do not free BT memory, enable keep-BLE-on, or externally deinit the manager.
 * - Manual manager cleanup joins transport users, clears ONLY SDK Wi-Fi keys
 *   via esp_wifi_restore(), restores RAM storage, then finishes networking.
 *   It never clears clock_net/RTC. SDK cleanup errors are logged, not reported
 *   as a rollback of an application config already saved and ACKed.
 *
 * Encrypted clock-control JSON protocol: docs/clock-control.md. Endpoint create
 * precedes start, register follows start; BLE discovers it using 0x2901.
 */
provision_result_t provision_run(bool initial);

#ifdef __cplusplus
}
#endif
