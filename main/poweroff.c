#include "poweroff.h"
#include "axp_prot.h"
#include "app_ui.h"
#include "rtc.h"
#include "ota.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static bool initialized, holding, confirmed, requested;
static int64_t pressed_at, sampled_at;

bool poweroff_init(void)
{
    holding = confirmed = requested = false;
    sampled_at = esp_timer_get_time();
    initialized = axp_power_key_begin();
    return initialized;
}

void poweroff_sample(void)
{
    if (!initialized || requested) return;
    int64_t now = esp_timer_get_time();
    // Frequent calls are cheap; PMU polling is capped at 50Hz.
    if (now - sampled_at < 20000) return;
    bool gap = now - sampled_at > 250000;
    sampled_at = now;
    uint8_t events;
    if (!axp_power_key_read(&events)) {
        holding = confirmed = false; // No inferred hold across failed read/ack.
        return;
    }
    // Once 4s was actually observed, a delayed release is still safe. Before
    // then, missed polls cannot establish continuity across blocking display IO.
    if (gap && !confirmed) holding = false;
    // POWERON is active low: positive=release, negative=press. Coalesced edges
    // are ambiguous and fail closed. Boot long IRQ alone never starts a hold.
    if (events & (0x01 | 0x08)) {
        if (confirmed && (events & 0x01) && !(events & (0x02 | 0x08)))
            requested = true;
        holding = confirmed = false;
    } else if (events & 0x02) {
        holding = true;
        confirmed = false;
        pressed_at = now;
    }
    if (holding && now - pressed_at >= 4000000) confirmed = true;
    // Never start a slow waveform with the key held: 10s emergency cutoff
    // remains enabled, including during OTA. Release is mandatory.

}

bool poweroff_requested(bool ota_protected)
{
    return requested && !ota_protected;
}

bool poweroff_tracking(void) { return holding || requested; }

bool poweroff_execute(void)
{
    // HTTP must already be stopped/joined by admin_run; no OTA can start now.
    if (!requested || ota_busy() || !app_ui_owner()) return false;
    // Release has been observed; leave the 10s emergency hardware cutoff ON.
    // Disable both the external RTC alarm and every configured ESP wake source.
    // Failure is recoverable without cutting power or proceeding to minute sleep.
    bool safe = rtc_cancel_alarm();
    esp_err_t err = esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (!safe || err != ESP_OK || !app_ui_poweroff()) goto failed;
    // app_ui_poweroff returns only AFTER the final white transfer's BUSY wait
    // and EPD_Sleep. No drawing or baseline cache write follows this point.
    if (!axp_power_off_checked()) goto failed;
    vTaskDelay(pdMS_TO_TICKS(1500)); // bounded shutdown confirmation window
    // Still executing means power removal wasn't established (e.g. VBUS).
failed:
    ESP_LOGE("poweroff", "Shutdown incomplete; staying awake, no minute wake; USB recovery or 10s PWR fallback");
    if (!axp_configure_power_key_fallback())
        ESP_LOGE("poweroff", "Could not restore hardware fallback; external power recovery required");
    return false;
}
