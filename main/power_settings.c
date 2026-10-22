#include "power_settings.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static SemaphoreHandle_t mutex;
// One atomic word publishes both fields together, including to the owner task.
static atomic_uint snapshot = 1000u << 1;
static _Atomic(const char *) warning = "";
// Protected by mutex; unknown persistence never suppresses a later retry.
static bool persisted_known;
static unsigned persisted;

static bool valid(power_settings_t s)
{
    return (s.sleep_mode == POWER_SLEEP_PMU || s.sleep_mode == POWER_SLEEP_DEEP) &&
        (s.pwr_on_ms == 128 || s.pwr_on_ms == 512 ||
         s.pwr_on_ms == 1000 || s.pwr_on_ms == 2000);
}
static unsigned pack(power_settings_t s)
{
    return (s.pwr_on_ms << 1) | (unsigned)s.sleep_mode;
}

void power_settings_init(void)
{
    // Startup/owner task only. Reinitialization reads but never resets NVS.
    if (!mutex) mutex = xSemaphoreCreateMutex();
    if (!mutex) {
        atomic_store(&warning, "电源设置内存不足，本次使用 PMU / 1000ms；无法保存。");
        return;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    power_settings_t s = {POWER_SLEEP_PMU, 1000};
    // Stable wire record: version, mode, threshold LE16, four reserved zeros.
    uint8_t record[8] = {0};
    size_t size = sizeof(record);
    nvs_handle_t h;
    esp_err_t err = nvs_open("clock_power", NVS_READONLY, &h);
    if (err == ESP_OK) {
        err = nvs_get_blob(h, "settings", record, &size);
        nvs_close(h);
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK; // Missing namespace/key is the persistent default.
    } else if (err == ESP_OK) {
        s.sleep_mode = (power_sleep_mode_t)record[1];
        s.pwr_on_ms = (unsigned)record[2] | ((unsigned)record[3] << 8);
        if (size != sizeof(record) || record[0] != 1 || record[4] ||
            record[5] || record[6] || record[7] || !valid(s))
            err = ESP_ERR_INVALID_STATE;
    }
    persisted_known = err == ESP_OK;
    if (persisted_known) {
        persisted = pack(s);
        atomic_store(&snapshot, persisted);
        atomic_store(&warning, "");
    } else {
        atomic_store(&snapshot, 1000u << 1);
        atomic_store(&warning, "无法读取电源设置，本次使用 PMU / 1000ms；请重新保存。");
    }
    xSemaphoreGive(mutex);
}

power_settings_t power_settings_get(void)
{
    unsigned value = atomic_load(&snapshot);
    return (power_settings_t){(power_sleep_mode_t)(value & 1u), value >> 1};
}

esp_err_t power_settings_save(power_settings_t s)
{
    if (!valid(s)) return ESP_ERR_INVALID_ARG;
    if (!mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(mutex, portMAX_DELAY);
    unsigned value = pack(s);
    esp_err_t err = ESP_OK;
    if (!persisted_known || persisted != value) {
        uint8_t record[8] = {1, (uint8_t)s.sleep_mode,
            (uint8_t)s.pwr_on_ms, (uint8_t)(s.pwr_on_ms >> 8), 0, 0, 0, 0};
        nvs_handle_t h;
        err = nvs_open("clock_power", NVS_READWRITE, &h);
        if (err == ESP_OK) {
            err = nvs_set_blob(h, "settings", record, sizeof(record));
            if (err == ESP_OK) err = nvs_commit(h);
            nvs_close(h);
        }
        persisted_known = err == ESP_OK;
    }
    if (err == ESP_OK) {
        persisted = value;
        atomic_store(&snapshot, value); // Publish only after successful commit.
        atomic_store(&warning, "");
    } else {
        atomic_store(&warning, "电源设置保存失败，仍保留之前的设置；请重试。");
    }
    xSemaphoreGive(mutex);
    return err;
}

const char *power_settings_warning(void)
{
    return atomic_load(&warning);
}
