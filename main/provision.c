#include "provision.h"
#include "app_runtime.h"
#include "display.h"
#include "network.h"
#include "rtc.h"
#include "sdkconfig.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bootloader_random.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"

#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_1)
#error "BLE provisioning requires BT and protocomm Security1 enabled"
#endif
#if !defined(CONFIG_BT_CONTROLLER_ENABLED) || (!defined(CONFIG_BT_NIMBLE_ENABLED) && !defined(CONFIG_BT_BLUEDROID_ENABLED))
#error "Select an enabled BLE controller and either NimBLE or Bluedroid host"
#endif
#if defined(CONFIG_WIFI_PROV_KEEP_BLE_ON_AFTER_PROV) || defined(CONFIG_ESP_PROTOCOMM_KEEP_BLE_ON_AFTER_BLE_STOP)
#error "Provisioning must stop/deinitialize the BLE controller after each session"
#endif
#if !defined(CONFIG_LOG_DYNAMIC_LEVEL_CONTROL) && CONFIG_LOG_MAXIMUM_LEVEL > 2
#error "Enable dynamic log control or cap logs at WARN to prevent SDK credential dumps"
#endif

#define SECOND_US 1000000LL
#define ACK_WINDOW_US (15 * SECOND_US)
/* UTC envelope; localtime must ALSO fit the PCF's 2000..2099 calendar. */
#define MIN_EPOCH 946684800LL
#define MAX_EPOCH 4102444799LL

static const char *TAG = "clock_prov";
static const char *CONTROL = "clock-control";

typedef enum {
    WIFI_IDLE, WIFI_PREPARING, WIFI_READY, WIFI_CONNECTING, WIFI_VERIFYING,
    WIFI_SAVED, WIFI_FAILED, WIFI_SAVE_ERROR, WIFI_PREPARE_ERROR
} wifi_phase_t;
static const char *const phase_names[] = {
    "idle", "preparing", "ready", "connecting", "verifying", "saved",
    "failed", "save_error", "prepare_error"
};

typedef struct {
    portMUX_TYPE lock;
    clock_wifi_config_t candidate;
    clock_wifi_config_t verified;
    wifi_phase_t phase;
    wifi_phase_t before_prepare;
    unsigned attempt;
    int64_t activity;
    bool have_candidate;
    bool pending_save;
    bool pending_prepare;
    bool configured;
    bool ever_saved;
    bool ended;
    bool exiting;
    /* Endpoint writes RTC on the transport thread, UI remains main-task only. */
    atomic_bool time_set;
    atomic_bool time_notice;
    atomic_bool ack_response;
} provision_session_t;

static void erase_secret(void *buffer, size_t size)
{
    volatile unsigned char *p = buffer;
    while (size--) *p++ = 0;
}

/* Synchronous SDK callback, including callbacks made with manager mutex held.
 * ONLY copy/flag under our lock: no NVS, display, or manager API calls. */
static void on_provision_event(void *arg, wifi_prov_cb_event_t id, void *data)
{
    provision_session_t *s = arg;
    portENTER_CRITICAL(&s->lock);
    if (!s->exiting) switch (id) {
    case WIFI_PROV_CRED_RECV:
        if (s->phase == WIFI_READY && data) {
            const wifi_sta_config_t *sta = data;
            memset(&s->candidate, 0, sizeof(s->candidate));
            memcpy(s->candidate.ssid, sta->ssid, sizeof(sta->ssid));
            memcpy(s->candidate.password, sta->password, sizeof(sta->password));
            s->have_candidate = s->candidate.ssid[0] != '\0';
            s->phase = WIFI_CONNECTING;
            s->activity = esp_timer_get_time();
        }
        break;
    case WIFI_PROV_CRED_FAIL:
        if (s->phase == WIFI_CONNECTING) {
            s->have_candidate = false;
            s->phase = WIFI_FAILED;
        }
        break;
    case WIFI_PROV_CRED_SUCCESS:
        /* IDF emits success on GOT_IP. This is a candidate, NOT a save ACK. */
        if (s->phase == WIFI_CONNECTING && s->have_candidate) {
            s->verified = s->candidate;
            s->pending_save = true;
            s->phase = WIFI_VERIFYING;
        }
        break;
    case WIFI_PROV_END:
        s->ended = true;
        break;
    default:
        break;
    }
    portEXIT_CRITICAL(&s->lock);
}

static void report_error(const char *operation, esp_err_t err)
{
    if (err != ESP_OK) ESP_LOGW(TAG, "%s: %s", operation, esp_err_to_name(err));
}

static esp_err_t json_reply(const char *text, uint8_t **out, ssize_t *outlen)
{
    size_t size = strlen(text);
    *out = malloc(size + 1); // protocomm owns and frees this allocation
    if (!*out) return ESP_ERR_NO_MEM;
    memcpy(*out, text, size + 1);
    *outlen = (ssize_t)size;
    return ESP_OK;
}

/* Registered with protocomm AFTER Security1 start. Unlike prov-session/version,
 * this is a normal encrypted endpoint. Never add a plaintext fallback. No
 * manager APIs here: prepare_wifi is an asynchronous main-loop mailbox. */
static esp_err_t clock_control(uint32_t session_id, const uint8_t *in, ssize_t len,
                               uint8_t **out, ssize_t *outlen, void *arg)
{
    (void)session_id;
    provision_session_t *s = arg;
    *out = NULL;
    *outlen = 0;
    char body[129];
    /* No escaped names/strings in this tiny protocol, including escaped NUL. */
    if (!in || len <= 0 || (size_t)len >= sizeof(body) || memchr(in, 0, len) || memchr(in, '\\', len))
        return json_reply("{\"ok\":false,\"error\":\"invalid_request\"}", out, outlen);
    memcpy(body, in, len);
    body[len] = 0;
    cJSON *json = cJSON_ParseWithOpts(body, NULL, true);
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(json, "op");
    int fields = cJSON_GetArraySize(json);
    if (!cJSON_IsObject(json) || !cJSON_IsString(op)) {
        cJSON_Delete(json);
        return json_reply("{\"ok\":false,\"error\":\"invalid_request\"}", out, outlen);
    }
    portENTER_CRITICAL(&s->lock);
    bool exiting = s->exiting;
    portEXIT_CRITICAL(&s->lock);
    const char *error = exiting ? "session_ending" : NULL;
    bool status = false;
    if (!error && !strcmp(op->valuestring, "status") && fields == 1) {
        status = true; // Background polling deliberately does NOT renew activity.
    } else if (!error && !strcmp(op->valuestring, "prepare_wifi") && fields == 1) {
        portENTER_CRITICAL(&s->lock);
        if (s->phase == WIFI_CONNECTING || s->phase == WIFI_VERIFYING) {
            error = "wifi_busy";
        } else if (s->phase != WIFI_PREPARING && s->phase != WIFI_READY) {
            s->before_prepare = s->phase;
            s->phase = WIFI_PREPARING;
            s->pending_prepare = true;
        }
        s->activity = esp_timer_get_time();
        portEXIT_CRITICAL(&s->lock);
        status = !error;
    } else if (!error && !strcmp(op->valuestring, "set_time") && fields == 2) {
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(json, "epoch");
        double n = cJSON_IsNumber(value) ? value->valuedouble : -1;
        time_t epoch = 0;
        struct tm local;
        if (!isfinite(n) || n < MIN_EPOCH || n > MAX_EPOCH || floor(n) != n) {
            error = "epoch_range";
        } else {
            epoch = (time_t)n;
            if (!localtime_r(&epoch, &local) || local.tm_year < 100 || local.tm_year > 199)
                error = "local_year_range";
        }
        if (!error) {
            /* Parent serializes rtc_read/rtc_set with the common RTC mutex.
             * rtc_set's void API aborts on I2C errors; no false recoverable ACK. */
            rtc_set(epoch);
            atomic_store(&s->time_set, true);
            atomic_store(&s->time_notice, true);
            portENTER_CRITICAL(&s->lock);
            s->activity = esp_timer_get_time();
            portEXIT_CRITICAL(&s->lock);
            status = true;
        }
    } else if (!error) {
        error = "invalid_request";
    }
    cJSON_Delete(json);
    char response[384];
    if (!status) {
        snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"%s\"}", error);
        return json_reply(response, out, outlen);
    }

    struct tm now = {0};
    bool valid = rtc_read(&now) && now.tm_year >= 100 && now.tm_year <= 199;
    char local_time[32] = "", epoch_text[32] = "null";
    if (valid) {
        struct tm normalized = now;
        normalized.tm_isdst = -1;
        time_t epoch = mktime(&normalized); // RTC stores device-local calendar.
        valid = epoch != (time_t)-1 && normalized.tm_year == now.tm_year &&
                normalized.tm_mon == now.tm_mon && normalized.tm_mday == now.tm_mday &&
                normalized.tm_hour == now.tm_hour && normalized.tm_min == now.tm_min &&
                normalized.tm_sec == now.tm_sec;
        if (valid) {
            strftime(local_time, sizeof(local_time), "%Y-%m-%d %H:%M:%S", &now);
            snprintf(epoch_text, sizeof(epoch_text), "%lld", (long long)epoch);
        }
    }
    portENTER_CRITICAL(&s->lock);
    wifi_phase_t phase = s->phase;
    unsigned attempt = s->attempt;
    bool configured = s->configured;
    portEXIT_CRITICAL(&s->lock);
    bool time_set = atomic_load(&s->time_set);
    snprintf(response, sizeof(response),
             "{\"ok\":true,\"wifi_configured\":%s,\"wifi_saved\":%s,"
             "\"wifi_state\":\"%s\",\"wifi_ready\":%s,\"wifi_attempt\":%u,"
             "\"rtc_valid\":%s,\"rtc_time\":\"%s\",\"epoch\":%s,\"time_set\":%s}",
             configured ? "true" : "false", phase == WIFI_SAVED ? "true" : "false",
             phase_names[phase], phase == WIFI_READY ? "true" : "false", attempt,
             valid ? "true" : "false", valid ? local_time : "", epoch_text,
             time_set ? "true" : "false");
    esp_err_t err = json_reply(response, out, outlen);
    if (err == ESP_OK && (phase == WIFI_SAVED || time_set)) {
        atomic_store(&s->ack_response, true);
    }
    return err;
}

/* UI task only. All main-loop manager calls are outside our lock. */
static void service_work(provision_session_t *s, const char *service_name, const char *pop)
{
    char code[32];
    snprintf(code, sizeof(code), "校验码：%s", pop);
    portENTER_CRITICAL(&s->lock);
    bool save = s->pending_save;
    s->pending_save = false;
    bool prepare = s->pending_prepare;
    s->pending_prepare = false;
    wifi_phase_t previous = s->before_prepare;
    portEXIT_CRITICAL(&s->lock);
    if (save) {
        esp_err_t err = network_save_config(&s->verified);
        report_error("Save verified configuration", err);
        portENTER_CRITICAL(&s->lock);
        s->phase = err == ESP_OK ? WIFI_SAVED : WIFI_SAVE_ERROR;
        if (err == ESP_OK) s->configured = s->ever_saved = true;
        erase_secret(&s->verified, sizeof(s->verified));
        erase_secret(&s->candidate, sizeof(s->candidate));
        s->have_candidate = false;
        portEXIT_CRITICAL(&s->lock);
        const char *lines[] = {err == ESP_OK ? "Wi-Fi 配置已保存" : "保存失败，保留旧配置",
                               "可继续在网页手动设置时间", service_name, code,
                               "长按中键选单"};
        app_ui_message("蓝牙设置", lines, 5);
    }
    if (prepare) {
        esp_err_t err = ESP_OK;
        if (previous == WIFI_FAILED) err = wifi_prov_mgr_reset_sm_state_on_failure();
        else if (previous != WIFI_IDLE)
            err = wifi_prov_mgr_reset_sm_state_for_reprovision();
        report_error("Prepare Wi-Fi submission", err);
        portENTER_CRITICAL(&s->lock);
        s->phase = err == ESP_OK ? WIFI_READY : WIFI_PREPARE_ERROR;
        if (err == ESP_OK) ++s->attempt;
        s->have_candidate = false;
        erase_secret(&s->candidate, sizeof(s->candidate));
        portEXIT_CRITICAL(&s->lock);
    }
    if (atomic_exchange(&s->time_notice, false)) {
        const char *lines[] = {"手动时间已写入 RTC", "不依赖 Wi-Fi 或 NTP",
                               service_name, code, "长按中键选单"};
        app_ui_message("蓝牙设置", lines, 5);
    }
}

static provision_result_t run_session(bool initial)
{
    provision_result_t result = {.next = APP_NONE};
    provision_session_t s = {
        .lock = portMUX_INITIALIZER_UNLOCKED,
        .phase = WIFI_READY,
        .attempt = 1, // Standard Espressif clients may submit the first config.
        .activity = esp_timer_get_time(),
    };
    atomic_init(&s.time_set, false);
    atomic_init(&s.time_notice, false);
    atomic_init(&s.ack_response, false);
    clock_wifi_config_t old;
    network_get_config(&old);
    s.configured = old.ssid[0] != '\0';
    erase_secret(&old, sizeof(old));
    bool prepared = false, manager = false;
    uint8_t *frame = NULL;
    char pop[7] = {0};
    char service_name[sizeof("PROV_EPAPER_") + 6] = {0};
    uint32_t random = 0;
    uint8_t mac[6];
    esp_err_t err;

    esp_log_level_set("wifi_prov_mgr", ESP_LOG_WARN);
    esp_log_level_set("wifi_prov_handlers", ESP_LOG_WARN);
    esp_log_level_set("security1", ESP_LOG_WARN);
    esp_log_level_set("wifi", ESP_LOG_WARN);
    frame = heap_caps_malloc(CLOCK_FRAME_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!frame) frame = heap_caps_malloc(CLOCK_FRAME_SIZE, MALLOC_CAP_8BIT);
    if (!frame) goto done;

    /* Radios must be off and no concurrent ADC use at this point. */
    bootloader_random_enable();
    esp_fill_random(&random, sizeof(random));
    bootloader_random_disable();
    snprintf(pop, sizeof(pop), "%06u", (unsigned)(random % 1000000));
    erase_secret(&random, sizeof(random));
    prepared = true; // Finish even a partially failed preparation.
    err = network_prepare_provisioning();
    if (err != ESP_OK) { report_error("Prepare Wi-Fi", err); goto done; }
    err = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (err != ESP_OK) { report_error("Read MAC", err); goto done; }
    snprintf(service_name, sizeof(service_name), "PROV_EPAPER_%02X%02X%02X", mac[3], mac[4], mac[5]);
    provision_render(frame, service_name, pop, initial);
    app_ui_present(frame, true); // UI retains background even with menu open.
    erase_secret(frame, CLOCK_FRAME_SIZE);
    free(frame);
    frame = NULL;

    wifi_prov_mgr_config_t config = {
        .scheme = wifi_prov_scheme_ble,
        /* NEVER FREE_BTDM/FREE_BLE/FREE_BT: repeated entry must work. */
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
        .app_event_handler = {.event_cb = on_provision_event, .user_data = &s},
        .wifi_prov_conn_cfg = {.wifi_conn_attempts = 3},
    };
    err = wifi_prov_mgr_init(config);
    if (err != ESP_OK) { report_error("Initialize provisioning", err); goto done; }
    manager = true;
    err = wifi_prov_mgr_disable_auto_stop(1000);
    if (err != ESP_OK) { report_error("Disable auto stop", err); goto done; }
    err = wifi_prov_mgr_endpoint_create(CONTROL); // Before start: BLE 0x2901 map.
    if (err != ESP_OK) { report_error("Create clock-control", err); goto done; }
    err = wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1, pop, service_name, NULL);
    if (err != ESP_OK) { report_error("Start BLE provisioning", err); goto done; }
    err = wifi_prov_mgr_endpoint_register(CONTROL, clock_control, &s);
    if (err != ESP_OK) { report_error("Register clock-control", err); goto done; }
    portENTER_CRITICAL(&s.lock);
    s.activity = esp_timer_get_time();
    portEXIT_CRITICAL(&s.lock);
    int64_t completed_at = 0, ack_at = 0;
    wifi_phase_t last_phase = WIFI_READY;
    for (;;) {
        bool activity = false, selected = false;
        app_action_t action = app_poll(&activity, &selected);
        if (action != APP_NONE) { result.next = action; break; }
        if (activity) {
            portENTER_CRITICAL(&s.lock);
            s.activity = esp_timer_get_time();
            portEXIT_CRITICAL(&s.lock);
        }
        service_work(&s, service_name, pop);
        int64_t now = esp_timer_get_time();
        portENTER_CRITICAL(&s.lock);
        bool ended = s.ended, success = s.ever_saved, configured = s.configured;
        wifi_phase_t phase = s.phase;
        int64_t last_activity = s.activity;
        portEXIT_CRITICAL(&s.lock);
        if (phase != last_phase && (phase == WIFI_FAILED || phase == WIFI_PREPARE_ERROR)) {
            char code[32];
            snprintf(code, sizeof(code), "校验码：%s", pop);
            const char *failure[] = {"Wi-Fi 连接或准备失败", "旧配置未覆盖，请在网页修改后重试", service_name, code, "长按中键打开选单"};
            app_ui_message(initial ? "仍在初始化" : "蓝牙管理", failure, 5);
        }
        last_phase = phase;
        // Setting time first must not end first-time setup before Wi-Fi was
        // configured; both forms can be completed in this one BLE session.
        success |= configured && atomic_load(&s.time_set);
        if (initial && success) {
            if (!completed_at) completed_at = now;
            if (!ack_at && atomic_load(&s.ack_response)) ack_at = now;
            /* A generated status response is not proof of delivery. Keep BLE
             * alive at least 2s afterwards, or a bounded 15s if never polled. */
            if ((ack_at && now - ack_at >= 2 * SECOND_US) || now - completed_at >= ACK_WINDOW_US) {
                result.next = APP_CLOCK;
                break;
            }
        } else if (now - last_activity >= (initial ? 295 : 300) * SECOND_US) {
            if (!initial) result.next = APP_CLOCK;
            break;
        }
        if (ended) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }

done:
    portENTER_CRITICAL(&s.lock);
    s.exiting = true;
    portEXIT_CRITICAL(&s.lock);
    /* Blocking transport join before stack-owned context/PoP is erased.
     * No wait() without timeout and no irreversible BT memory release. */
    if (manager) wifi_prov_mgr_deinit();
    if (prepared) {
        /* IDF start/configure_sta forces FLASH. Clear SDK Wi-Fi keys only;
         * clock_net commits have already been ACKed and must not be erased. */
        report_error("Clear Wi-Fi stack persistence", esp_wifi_restore());
        report_error("Restore RAM Wi-Fi storage", esp_wifi_set_storage(WIFI_STORAGE_RAM));
        network_finish_provisioning();
    }
    result.wifi_saved = s.ever_saved;
    result.time_set = atomic_load(&s.time_set);
    if (initial && result.next == APP_NONE &&
        (result.wifi_saved || (s.configured && result.time_set)))
        result.next = APP_CLOCK;
    if (frame) { erase_secret(frame, CLOCK_FRAME_SIZE); free(frame); }
    erase_secret(pop, sizeof(pop));
    erase_secret(&random, sizeof(random));
    erase_secret(&s.candidate, sizeof(s.candidate));
    erase_secret(&s.verified, sizeof(s.verified));
    return result;
}

provision_result_t provision_run(bool initial)
{
    for (;;) {
        provision_result_t result = run_session(initial);
        if (result.next != APP_NONE) return result;
        if (!initial) { result.next = APP_CLOCK; return result; }
        /* Forced initialization never exits on old SSID/valid RTC, error, or
         * radio timeout. No busy retry loop on SDK failures: require select. */
        const char *lines[] = {"仍在初始化", "中键重试", "长按中键选单"};
        app_ui_message("初始化", lines, 3);
        for (;;) {
            bool activity = false, selected = false;
            app_action_t action = app_poll(&activity, &selected);
            if (action != APP_NONE) { result.next = action; return result; }
            if (selected) break;
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}
