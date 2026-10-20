#include "render_settings.h"
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static SemaphoreHandle_t mutex;
static atomic_int requested = RENDER_MONO;
static atomic_uint request_revision;
static unsigned applied_revision; // UI task only.
static _Atomic(const char *) warning = "";
// Protected by mutex. Missing key is the persistent default mono and does not
// need a write. A failed read/commit is unknown and must not suppress a retry.
static bool persisted_known;
static render_mode_t persisted;

static bool valid_mode(render_mode_t mode)
{
    return mode == RENDER_MONO || mode == RENDER_LINEAR || mode == RENDER_INK_MATCH;
}

void render_settings_init(void)
{
    if (mutex) return;
    mutex = xSemaphoreCreateMutex();
    if (!mutex) {
        atomic_store(&warning, "显示设置内存不足，本次使用无灰度。");
        render_mode_set(RENDER_MONO);
        return;
    }
    nvs_handle_t handle;
    uint8_t value = RENDER_MONO;
    esp_err_t err = nvs_open("clock_ui", NVS_READONLY, &handle);
    if (err == ESP_OK) {
        err = nvs_get_u8(handle, "render", &value);
        nvs_close(handle);
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        value = RENDER_MONO;
        err = ESP_OK;
    }
    if (err == ESP_OK && valid_mode((render_mode_t)value)) {
        persisted = (render_mode_t)value;
        persisted_known = true;
        atomic_store(&requested, persisted);
    } else {
        atomic_store(&warning, "无法读取显示设置，本次使用无灰度；可重新保存设置。");
    }
    render_mode_set(render_settings_requested());
}

// Caller holds mutex. Never opens any Wi-Fi/config/RTC namespace.
static esp_err_t persist(render_mode_t mode)
{
    if (persisted_known && persisted == mode) return ESP_OK;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("clock_ui", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, "render", (uint8_t)mode);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) {
        persisted = mode;
        persisted_known = true;
    } else {
        persisted_known = false;
    }
    return err;
}

esp_err_t render_settings_save(render_mode_t mode)
{
    if (!valid_mode(mode)) return ESP_ERR_INVALID_ARG;
    if (!mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(mutex, portMAX_DELAY);
    esp_err_t err = persist(mode);
    if (err == ESP_OK) {
        atomic_store(&requested, mode);
        atomic_fetch_add(&request_revision, 1);
        // An explicit save also re-renders an unchanged mode after a fallback.
        atomic_store(&warning, "");
    }
    xSemaphoreGive(mutex);
    return err;
}

render_mode_t render_settings_requested(void)
{
    return (render_mode_t)atomic_load(&requested);
}

bool render_settings_apply(void)
{
    render_mode_t mode = render_settings_requested();
    unsigned revision = atomic_load(&request_revision);
    if (render_mode_get() == mode && applied_revision == revision) return false;
    render_mode_set(mode);
    applied_revision = revision;
    return true;
}

static esp_err_t restore_mono(const char *saved, const char *unsaved)
{
    if (mutex) xSemaphoreTake(mutex, portMAX_DELAY);
    esp_err_t err = mutex ? persist(RENDER_MONO) : ESP_ERR_INVALID_STATE;
    // Publish mono even on NVS failure: do not reapply the failing gray request.
    atomic_store(&requested, RENDER_MONO);
    render_mode_set(RENDER_MONO);
    applied_revision = atomic_load(&request_revision);
    atomic_store(&warning, err == ESP_OK ? saved : unsaved);
    if (mutex) xSemaphoreGive(mutex);
    return err;
}

esp_err_t render_settings_fallback(void)
{
    return restore_mono("灰阶刷新超时，已恢复无灰度并保存。",
        "灰阶刷新超时，本次已恢复无灰度；回退设置保存失败，重启后可能重试灰阶。");
}

esp_err_t render_settings_restore_default(void)
{
    return restore_mono("强制初始化已恢复无灰度。", "本次强制初始化使用无灰度，但默认模式保存失败。");
}

const char *render_settings_warning(void)
{
    return atomic_load(&warning);
}
