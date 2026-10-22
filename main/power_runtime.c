#include "power_runtime.h"
#include "power_settings.h"
#include "axp_prot.h"
#include "rtc.h"
#include "epaper_port.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CENTER_GPIO GPIO_NUM_5
#define CENTER_MASK (UINT64_C(1) << CENTER_GPIO)

power_wake_t power_runtime_wake(bool rtc_alarm)
{
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_TIMER || cause == ESP_SLEEP_WAKEUP_EXT1) {
        bool center = cause == ESP_SLEEP_WAKEUP_EXT1 &&
            (esp_sleep_get_ext1_wakeup_status() & CENTER_MASK);
        // EXT1 switches the pad to RTC IO. Restore digital input scanning on
        // BOTH timer and key wakes; GPIO45 is not an S3 RTC wake-capable pad.
        ESP_ERROR_CHECK(rtc_gpio_hold_dis(CENTER_GPIO));
        ESP_ERROR_CHECK(rtc_gpio_deinit(CENTER_GPIO));
        return center ? POWER_WAKE_CENTER :
            cause == ESP_SLEEP_WAKEUP_TIMER ? POWER_WAKE_SCHEDULED : POWER_WAKE_MANUAL;
    }
    // Only the original PMU/cold-boot (UNDEFINED) path uses the RTC flag;
    // a deep-wake cause above always takes precedence over a stale alarm.
    return cause == ESP_SLEEP_WAKEUP_UNDEFINED && rtc_alarm ?
        POWER_WAKE_SCHEDULED : POWER_WAKE_MANUAL;
}

void power_runtime_apply_settings(void)
{
    static unsigned applied_ms = 1000; // axp_init's unchanged safe default
    static int64_t retry_after;
    power_settings_t settings = power_settings_get();
    if (settings.pwr_on_ms == applied_ms) return;
    int64_t now = esp_timer_get_time();
    if (now < retry_after) return;
    if (axp_set_power_on_time(settings.pwr_on_ms)) {
        applied_ms = settings.pwr_on_ms;
        retry_after = 0;
    } else {
        ESP_LOGW("power", "PWR timing apply failed; will retry on owner task");
        retry_after = now + 1000000;
    }
}

uint64_t power_runtime_minute_us(bool valid, int second)
{
    return (uint64_t)(valid && second >= 0 && second < 60 ? 60 - second : 60) * 1000000;
}

void power_runtime_sleep(void)
{
    power_settings_t settings = power_settings_get();
    if (get_usb_connected()) return;
    if (settings.sleep_mode != POWER_SLEEP_DEEP) {
        rtc_arm_next_minute();
        axp_pwr_off();
        // Preserve the default PMU behavior, including no further I2C traffic
        // if the shutdown command has not yet removed power.
        vTaskDelay(portMAX_DELAY);
        return;
    }

    // Never enter level-triggered EXT1 with an already-held center button.
    // A second sample also rejects release bounce; scanner retains boot ignore.
    if (!gpio_get_level(CENTER_GPIO)) return;
    vTaskDelay(pdMS_TO_TICKS(40));
    if (!gpio_get_level(CENTER_GPIO) || get_usb_connected()) return;

    // Display sleep also disables ALDO3; DC1/ESP32 power remains enabled.
    EPD_Sleep();
    struct tm now;
    bool valid = rtc_read(&now);
    esp_err_t err = rtc_gpio_pullup_en(CENTER_GPIO);
    if (err == ESP_OK) err = rtc_gpio_pulldown_dis(CENTER_GPIO);
    if (err == ESP_OK) err = esp_sleep_disable_ext1_wakeup_io(0);
    if (err == ESP_OK) err = esp_sleep_enable_ext1_wakeup_io(CENTER_MASK, ESP_EXT1_WAKEUP_ANY_LOW);
    if (err == ESP_OK) err = esp_sleep_enable_timer_wakeup(power_runtime_minute_us(valid, now.tm_sec));
    if (err != ESP_OK) {
        ESP_LOGE("power", "Deep sleep setup failed: %s; staying awake", esp_err_to_name(err));
        return;
    }
    // SDK 5.5.2 holds RTC pull configuration for EXT1 with RTC_PERIPH=AUTO.
    // GPIO mux/input selection happens inside esp_deep_sleep_start(). A press
    // racing this last check may wake once immediately, then waits for release
    // on the next boot rather than looping while the button remains held.
    if (!gpio_get_level(CENTER_GPIO) || get_usb_connected()) return;
    esp_deep_sleep_start();
}
