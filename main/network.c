#include "network.h"
#include "rtc.h"
#include "sdkconfig.h"

#include <assert.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "nvs.h"

#define CONNECTED BIT0
#define CONNECT_FAILED BIT1
#define STA_STOPPED BIT2
#define STA_WAIT pdMS_TO_TICKS(15000)
#define NTP_WAIT pdMS_TO_TICKS(10000)
#define POLL_SLICE pdMS_TO_TICKS(100)
#define CONFIG_VERSION 1U

// One NVS item contains credentials and their schedule state, so a new config
// and resetting its daily attempt cannot be partially committed. Legacy state
// remains in its old keys until an explicit successful network_save_config().
typedef struct {
    uint32_t version;
    int32_t attempt_day;
    int64_t sync_epoch;
    clock_wifi_config_t wifi;
    uint8_t reserved[6];
} stored_config_t;
_Static_assert(sizeof(stored_config_t) == 120, "Network config blob layout changed");

static const char *TAG = "clock_net";
// NVS keys are limited to 15 characters; "last_attempt_day" is too long.
static const char *ATTEMPT_KEY = "last_attempt";
static clock_wifi_config_t saved;
static int32_t last_attempt_day;
static int64_t last_sync_epoch;
static EventGroupHandle_t events;
static esp_netif_t *sta_netif;
static esp_event_handler_instance_t wifi_handler, ip_handler;
static bool driver_initialized, wifi_running, blob_settings;
static atomic_bool cancelled;
typedef bool (*poll_hook_t)(void);
static _Atomic(poll_hook_t) poll_hook;

void network_set_poll_hook(bool (*hook)(void))
{
    atomic_store(&poll_hook, hook);
}

void network_cancel(void)
{
    atomic_store(&cancelled, true);
}

void network_clear_cancel(void)
{
    atomic_store(&cancelled, false);
}

static bool poll_cancel(void)
{
    poll_hook_t hook = atomic_load(&poll_hook);
    if (hook && hook()) network_cancel();
    return atomic_load(&cancelled);
}

// Public operations are serialized by the caller, except cancellation and hook
// registration. The event task shares only event bits, never config/state.
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP) {
            xEventGroupClearBits(events, CONNECT_FAILED);
            xEventGroupSetBits(events, CONNECTED);
        } else if (id == IP_EVENT_STA_LOST_IP) {
            xEventGroupClearBits(events, CONNECTED);
            xEventGroupSetBits(events, CONNECT_FAILED);
        }
    } else if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_DISCONNECTED) {
            xEventGroupClearBits(events, CONNECTED);
            xEventGroupSetBits(events, CONNECT_FAILED);
            // Deliberately no reconnect loop here.
        } else if (id == WIFI_EVENT_STA_STOP) {
            xEventGroupClearBits(events, CONNECTED);
            xEventGroupSetBits(events, STA_STOPPED);
        }
    }
}

static int32_t local_day(const struct tm *t)
{
    return (t->tm_year + 1900) * 10000 + (t->tm_mon + 1) * 100 + t->tm_mday;
}

static bool valid_config(const clock_wifi_config_t *config)
{
    if (!config) return false;
    size_t ssid_len = strnlen(config->ssid, sizeof(config->ssid));
    size_t pass_len = strnlen(config->password, sizeof(config->password));
    if (ssid_len > 32 || pass_len > 64) return false;
    if (!ssid_len) return pass_len == 0; // Allow explicitly clearing credentials.
    if (pass_len && pass_len < 8) return false;
    if (pass_len == 64 && strspn(config->password, "0123456789abcdefABCDEF") != 64)
        return false;
    return true;
}

static void load_settings(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("clock_net", NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Read settings: %s", esp_err_to_name(err));
        return;
    }
    stored_config_t record = {0};
    size_t size = sizeof(record);
    err = nvs_get_blob(handle, "config", &record, &size);
    if (err == ESP_OK && size == sizeof(record) &&
        record.version == CONFIG_VERSION && valid_config(&record.wifi)) {
        saved = record.wifi;
        last_attempt_day = record.attempt_day;
        last_sync_epoch = record.sync_epoch;
        blob_settings = true;
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        // Read-only compatibility: never migrate merely because we booted.
        clock_wifi_config_t config = {0};
        size = sizeof(config.ssid);
        err = nvs_get_str(handle, "ssid", config.ssid, &size);
        if (err == ESP_OK) {
            size = sizeof(config.password);
            err = nvs_get_str(handle, "password", config.password, &size);
            if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK; // Open network.
            if (err == ESP_OK && valid_config(&config)) saved = config;
        }
        if (nvs_get_i32(handle, ATTEMPT_KEY, &last_attempt_day) != ESP_OK)
            last_attempt_day = 0;
        if (nvs_get_i64(handle, "last_sync_epoch", &last_sync_epoch) != ESP_OK)
            last_sync_epoch = 0;
    } else {
        // Never resurrect stale legacy credentials after an invalid/newer blob.
        ESP_LOGW(TAG, "Invalid or unsupported network config blob");
    }
    nvs_close(handle);
}

static esp_err_t store_record(nvs_handle_t handle,
                              const clock_wifi_config_t *config, int32_t day)
{
    stored_config_t record = {0};
    record.version = CONFIG_VERSION;
    record.attempt_day = day;
    record.sync_epoch = last_sync_epoch;
    record.wifi = *config;
    return nvs_set_blob(handle, "config", &record, sizeof(record));
}

static esp_err_t persist_sync_state(bool success)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("clock_net", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    if (blob_settings) {
        err = store_record(handle, &saved, last_attempt_day);
    } else {
        err = nvs_set_i32(handle, ATTEMPT_KEY, last_attempt_day);
        if (err == ESP_OK && success)
            err = nvs_set_i64(handle, "last_sync_epoch", last_sync_epoch);
    }
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

void network_init(void)
{
    load_settings(); // NVS initialization belongs to main, not this module.
    events = xEventGroupCreate();
    assert(events);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    // Netifs and event handlers live for this boot; only the radio is cycled.
    sta_netif = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                       on_event, NULL, &wifi_handler));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                                       on_event, NULL, &ip_handler));
    // No esp_wifi_init/start here: radio remains off.
}

void network_get_config(clock_wifi_config_t *config)
{
    if (config) *config = saved;
}

esp_err_t network_save_config(const clock_wifi_config_t *config)
{
    if (!valid_config(config)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("clock_net", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = store_record(handle, config, 0);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) {
        saved = *config;
        last_attempt_day = 0;
        blob_settings = true;
    }
    // Do not change a live station: management exits before using new settings.
    return err;
}

static esp_err_t prepare_driver(void)
{
    if (driver_initialized) return ESP_OK;
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&config);
    if (err != ESP_OK) return err;
    driver_initialized = true;
    // Credentials are owned exclusively by clock_net; never persist in Wi-Fi NVS.
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) network_stop();
    return err;
}

static esp_err_t configure_station(void)
{
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, saved.ssid, strlen(saved.ssid));
    memcpy(config.sta.password, saved.password, strlen(saved.password));
    config.sta.failure_retry_cnt = 0;
    return esp_wifi_set_config(WIFI_IF_STA, &config);
}

static esp_err_t start_station(void)
{
    // A running station may be reconnecting. Never set_config in that state;
    // changed saved credentials take effect only after the caller stops Wi-Fi.
    if (wifi_running) return ESP_OK;
    esp_err_t err = prepare_driver();
    if (err != ESP_OK) return err;
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = configure_station();
    if (err == ESP_OK && !wifi_running) {
        xEventGroupClearBits(events, CONNECTED | CONNECT_FAILED | STA_STOPPED);
        err = esp_wifi_start();
        if (err == ESP_OK) wifi_running = true;
    }
    return err;
}

static esp_err_t connect_station(void)
{
    if (network_connected()) return ESP_OK;
    xEventGroupClearBits(events, CONNECTED | CONNECT_FAILED);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) return err;
    TickType_t started = xTaskGetTickCount();
    for (;;) {
        if (poll_cancel()) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }
        EventBits_t bits = xEventGroupGetBits(events);
        if ((bits & CONNECTED) && !(bits & CONNECT_FAILED)) return ESP_OK;
        if (bits & CONNECT_FAILED) {
            err = ESP_FAIL;
            break;
        }
        TickType_t elapsed = xTaskGetTickCount() - started;
        if (elapsed >= STA_WAIT) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        TickType_t remaining = STA_WAIT - elapsed;
        xEventGroupWaitBits(events, CONNECTED | CONNECT_FAILED, pdFALSE, pdFALSE,
                            remaining < POLL_SLICE ? remaining : POLL_SLICE);
    }
    // Cancel slow association/DHCP too; never retry in an event handler.
    esp_wifi_disconnect();
    xEventGroupClearBits(events, CONNECTED);
    return err;
}

bool network_configured(void)
{
    return saved.ssid[0] != '\0';
}

esp_err_t network_connect(void)
{
    if (poll_cancel()) return ESP_ERR_INVALID_STATE;
    if (!network_configured()) return ESP_ERR_NOT_FOUND;
    if (network_connected()) return ESP_OK;
    esp_err_t err = start_station();
    if (err == ESP_OK) {
        if (poll_cancel()) return ESP_ERR_INVALID_STATE;
        err = connect_station();
    }
    return err;
}

bool network_connected(void)
{
    return events && (xEventGroupGetBits(events) & CONNECTED) != 0;
}

void network_station_ip(char *ip, unsigned size)
{
    if (!ip || !size) return;
    ip[0] = '\0';
    esp_netif_ip_info_t info;
    if (sta_netif && network_connected() &&
        esp_netif_get_ip_info(sta_netif, &info) == ESP_OK)
        snprintf(ip, size, IPSTR, IP2STR(&info.ip));
}

esp_err_t network_query_time(time_t *epoch)
{
    if (!epoch) return ESP_ERR_INVALID_ARG;
    esp_err_t err = network_connect();
    if (err != ESP_OK) return err;
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_CLOCK_NTP_SERVER);
    config.smooth_sync = false;
    config.wait_for_sync = true;
    err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) return err;
    // Each query owns a new SNTP instance/semaphore: an old system clock or
    // previous response cannot satisfy this wait. SNTP still sets system time.
    TickType_t started = xTaskGetTickCount();
    for (;;) {
        if (poll_cancel()) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }
        if (!network_connected()) {
            err = ESP_FAIL;
            break;
        }
        // A slow menu refresh must not discard a response already queued by
        // the SNTP worker while the UI task was drawing the e-paper.
        err = esp_netif_sntp_sync_wait(0);
        if (err != ESP_ERR_TIMEOUT) break;
        TickType_t elapsed = xTaskGetTickCount() - started;
        if (elapsed >= NTP_WAIT) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        TickType_t remaining = NTP_WAIT - elapsed;
        err = esp_netif_sntp_sync_wait(remaining < POLL_SLICE ? remaining : POLL_SLICE);
        if (err != ESP_ERR_TIMEOUT) break;
    }
    esp_netif_sntp_deinit();
    if (poll_cancel()) return ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) *epoch = time(NULL);
    // Diagnostics never touch PCF RTC, daily-attempt state, or NVS.
    return err;
}

esp_err_t network_sync_now(void)
{
    time_t epoch;
    esp_err_t err = network_query_time(&epoch);
    if (err == ESP_OK) {
        struct tm corrected;
        // PCF85063 stores only years 2000..2099. Reject unrepresentable dates.
        if (!localtime_r(&epoch, &corrected) || corrected.tm_year < 100 ||
            corrected.tm_year > 199) {
            err = ESP_ERR_INVALID_RESPONSE;
        } else {
            // Last cancellation boundary before the RTC/state commit. A cancel
            // racing with the actual write cannot roll back an RTC update.
            if (atomic_load(&cancelled)) return ESP_ERR_INVALID_STATE;
            rtc_set(epoch);
            last_sync_epoch = (int64_t)epoch;
            last_attempt_day = local_day(&corrected);
            esp_err_t stored = persist_sync_state(true);
            if (stored != ESP_OK)
                ESP_LOGW(TAG, "Persist sync result: %s", esp_err_to_name(stored));
            // RTC really changed even if NVS failed: callers must full-refresh.
        }
    }
    // The caller owns radio lifetime; HTTP sync keeps its LAN connection.
    return err;
}

bool network_daily_sync(void)
{
    struct tm now;
    if (poll_cancel() || !saved.ssid[0] || !rtc_read(&now)) return false;
    if (now.tm_hour < CONFIG_CLOCK_SYNC_HOUR) return false;
    int32_t day = local_day(&now);
    if (day == last_attempt_day) return false;
    // Persist BEFORE any networking, so failed attempts and power cycles cannot
    // turn the once-daily schedule into a once-per-minute retry loop.
    last_attempt_day = day;
    esp_err_t err = persist_sync_state(false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Persist daily attempt: %s", esp_err_to_name(err));
        network_stop();
        return false;
    }
    err = network_sync_now();
    network_stop(); // Automatic attempts always return with the radio off.
    if (err != ESP_OK) ESP_LOGW(TAG, "Daily sync: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

esp_err_t network_start_management(clock_management_info_t *info)
{
    if (!info) return ESP_ERR_INVALID_ARG;
    esp_err_t err = network_connect();
    if (err != ESP_OK) {
        network_stop();
        return err;
    }
    network_station_ip(info->ip, sizeof(info->ip));
    return ESP_OK;
}

esp_err_t network_prepare_provisioning(void)
{
    if (poll_cancel()) return ESP_ERR_INVALID_STATE;
    network_stop();
    if (poll_cancel()) return ESP_ERR_INVALID_STATE;
    esp_err_t err = prepare_driver();
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    return err;
}

void network_finish_provisioning(void)
{
    // The provisioning manager may have started the driver independently.
    // network_stop deliberately stops an initialized driver unconditionally.
    network_stop();
}

void network_stop(void)
{
    if (!driver_initialized) return;
    xEventGroupClearBits(events, STA_STOPPED);
    esp_err_t stopped = esp_wifi_stop();
    if (stopped == ESP_OK) {
        TickType_t started = xTaskGetTickCount();
        const TickType_t budget = pdMS_TO_TICKS(1000);
        while (!(xEventGroupGetBits(events) & STA_STOPPED)) {
            // Cleanup is mandatory even when cancelled; still service the UI.
            (void)poll_cancel();
            TickType_t elapsed = xTaskGetTickCount() - started;
            if (elapsed >= budget) break;
            TickType_t remaining = budget - elapsed;
            xEventGroupWaitBits(events, STA_STOPPED, pdFALSE, pdTRUE,
                                remaining < POLL_SLICE ? remaining : POLL_SLICE);
        }
    } else if (stopped != ESP_ERR_WIFI_NOT_STARTED)
        ESP_LOGW(TAG, "Stop Wi-Fi: %s", esp_err_to_name(stopped));
    if (stopped == ESP_OK || stopped == ESP_ERR_WIFI_NOT_STARTED)
        wifi_running = false;
    esp_err_t err = esp_wifi_deinit();
    if (err != ESP_OK) ESP_LOGW(TAG, "Release Wi-Fi: %s", esp_err_to_name(err));
    else driver_initialized = wifi_running = false;
    xEventGroupClearBits(events, CONNECTED | CONNECT_FAILED);
}

int64_t network_last_sync(void)
{
    return last_sync_epoch;
}
