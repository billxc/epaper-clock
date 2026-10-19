#include "provision.h"
#include "display.h"
#include "network.h"
#include "epaper_port.h"
#include "sdkconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bootloader_random.h"
#include "esp_event.h"
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

/* Display integration is implemented by the parent change in display.c. */
void provision_render(uint8_t *frame, const char *service_name, const char *pop);

#define SESSION_US (300LL * 1000000)
#define TEARDOWN_RESERVE_US (5LL * 1000000)

static const char *TAG = "clock_prov";

typedef struct {
    portMUX_TYPE lock;
    clock_wifi_config_t candidate;
    clock_wifi_config_t verified;
    int64_t deadline;
    bool have_candidate;
    bool success;
    bool retry;
    bool ended;
} provision_session_t;

static void erase_secret(void *buffer, size_t size)
{
    volatile unsigned char *p = buffer;
    while (size--) *p++ = 0;
}

/* Synchronous manager app callback (may run on BLE or event-loop tasks).
 * Never stop/deinit the manager, do NVS/EPD work, or call a manager API here.
 * Unlike subscribing to queued WIFI_PROV_EVENT, this cannot consume stale END
 * events from an earlier invocation. Manager/transport teardown joins users
 * before this session's stack storage is released. Fixed-width SSID/PSK works. */
static void on_provision_event(void *arg, wifi_prov_cb_event_t id, void *data)
{
    provision_session_t *s = arg;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s->lock);
    switch (id) {
    case WIFI_PROV_CRED_RECV:
        if (!s->success && data) {
            const wifi_sta_config_t *sta = data;
            memset(&s->candidate, 0, sizeof(s->candidate));
            memcpy(s->candidate.ssid, sta->ssid, sizeof(sta->ssid));
            memcpy(s->candidate.password, sta->password, sizeof(sta->password));
            s->have_candidate = s->candidate.ssid[0] != '\0';
        }
        break;
    case WIFI_PROV_CRED_FAIL:
        if (!s->success) {
            s->have_candidate = false;
            s->retry = true;
        }
        break;
    case WIFI_PROV_CRED_SUCCESS:
        /* IDF emits success on IP_EVENT_STA_GOT_IP, not merely association. */
        if (!s->success && s->have_candidate && now < s->deadline) {
            s->verified = s->candidate;
            s->success = true;
            s->retry = false;
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

bool provision_run(void)
{
    provision_session_t s = {
        .lock = portMUX_INITIALIZER_UNLOCKED,
        .deadline = esp_timer_get_time() + SESSION_US - TEARDOWN_RESERVE_US,
    };
    bool prepared = false, manager = false, saved = false;
    uint8_t *frame = NULL;
    char pop[17] = {0};
    char service_name[sizeof("PROV_EPAPER_") + 6] = {0};
    uint8_t random[8] = {0}, mac[6];
    esp_err_t err;

    /* IDF manager prints SSIDs/partially masked passwords at DEBUG; Security1
     * can dump handshake/key material. Do not restore these tag levels later.
     * Production's global WARN setting also suppresses Wi-Fi stack SSID logs. */
    esp_log_level_set("wifi_prov_mgr", ESP_LOG_WARN);
    esp_log_level_set("wifi_prov_handlers", ESP_LOG_WARN);
    esp_log_level_set("security1", ESP_LOG_WARN);
    esp_log_level_set("wifi", ESP_LOG_WARN);

    frame = heap_caps_malloc(CLOCK_FRAME_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!frame) frame = heap_caps_malloc(CLOCK_FRAME_SIZE, MALLOC_CAP_8BIT);
    if (!frame) goto done;

    /* Must precede Wi-Fi/BT initialization. With radios off, esp_fill_random()
     * alone does not guarantee fresh physical entropy. No concurrent ADC use. */
    bootloader_random_enable();
    esp_fill_random(random, sizeof(random));
    bootloader_random_disable();
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < sizeof(random); ++i) {
        pop[2 * i] = hex[random[i] >> 4];
        pop[2 * i + 1] = hex[random[i] & 15];
    }
    erase_secret(random, sizeof(random));

    prepared = true; // Also finish a partially failed preparation.
    err = network_prepare_provisioning();
    if (err != ESP_OK) {
        report_error("Prepare Wi-Fi", err);
        goto done;
    }
    err = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (err != ESP_OK) {
        report_error("Read MAC", err);
        goto done;
    }
    snprintf(service_name, sizeof(service_name), "PROV_EPAPER_%02X%02X%02X",
             mac[3], mac[4], mac[5]);

    /* epaper_port_init belongs to main. Never contaminate the clock cache. */
    provision_render(frame, service_name, pop);
    EPD_Init();
    EPD_Display_Base(frame);
    EPD_Sleep();
    erase_secret(frame, CLOCK_FRAME_SIZE);
    free(frame);
    frame = NULL;
    if (esp_timer_get_time() >= s.deadline) goto done;

    wifi_prov_mgr_config_t config = {
        .scheme = wifi_prov_scheme_ble,
        /* NEVER FREE_BTDM/FREE_BLE/FREE_BT: entry must work again this boot. */
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
        .app_event_handler = {.event_cb = on_provision_event, .user_data = &s},
        /* IDF 5.5.2's counter is cumulative across failure-state resets; after
         * these retries later submissions still get at least one attempt. */
        .wifi_prov_conn_cfg = {.wifi_conn_attempts = 3},
    };
    err = wifi_prov_mgr_init(config);
    if (err != ESP_OK) {
        report_error("Initialize provisioning", err);
        goto done;
    }
    manager = true;
    /* Always start, even if Wi-Fi's NVS reports 'provisioned'. clock_net is the
     * only application configuration source. Use the default BLE service UUID.
     * Keep auto-stop: the client can query Connected before stop, or the SDK
     * stops after CONFIG_WIFI_PROV_AUTOSTOP_TIMEOUT (normally 30 seconds). */
    err = wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1, pop, service_name, NULL);
    if (err != ESP_OK) {
        report_error("Start BLE provisioning", err);
        goto done;
    }

    while (esp_timer_get_time() < s.deadline) {
        portENTER_CRITICAL(&s.lock);
        bool ended = s.ended;
        bool retry = s.retry && !s.success;
        s.retry = false;
        portEXIT_CRITICAL(&s.lock);
        if (ended) break;
        if (retry) {
            /* Finite connection attempts cover all disconnect reasons. Reset
             * after failure permits another submission without restarting BLE.
             * Never call reset_provisioning(): it is unrelated to clock_net. */
            err = wifi_prov_mgr_reset_sm_state_on_failure();
            if (err != ESP_OK) {
                report_error("Reset failed provisioning attempt", err);
                goto done;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

done:
    /* Blocking join, also on timeout/start failure. If still active, deinit
     * stops through scheme_ble.prov_stop -> protocomm_ble_stop. IDF 5.5.2
     * supports both hosts: Bluedroid's simple_ble_stop disables/deinits the
     * controller; NimBLE uses nimble_port_stop + nimble_port_deinit, which also
     * disables/deinits the controller. Neither path releases S3 BT memory.
     * Do NOT use wifi_prov_mgr_wait(): it has no timeout.
     * Let the auto-stop event finish, but never deinit from its callback. */
    if (manager) wifi_prov_mgr_deinit();
    if (prepared) {
        /* v5.5.2 start/configure_sta force WIFI_STORAGE_FLASH. Setting RAM in
         * prepare or SET_STA_CONFIG cannot prevent that. Once the manager is
         * joined, clear ONLY Wi-Fi stack persistence (not nvs_flash_erase nor
         * clock_net), then restore RAM policy before network tears driver down.
         * This also removes rejected/timed-out credentials written by manager.
         * On cleanup failure, do not commit new application credentials. */
        esp_err_t cleared = esp_wifi_restore();
        esp_err_t ram = esp_wifi_set_storage(WIFI_STORAGE_RAM);
        report_error("Clear Wi-Fi stack persistence", cleared);
        report_error("Restore RAM Wi-Fi storage", ram);
        network_finish_provisioning();
        if (s.success && cleared == ESP_OK && ram == ESP_OK) {
            err = network_save_config(&s.verified);
            saved = err == ESP_OK;
            report_error("Save verified configuration", err);
        }
    }
    if (frame) {
        erase_secret(frame, CLOCK_FRAME_SIZE);
        free(frame);
    }
    erase_secret(pop, sizeof(pop));
    erase_secret(random, sizeof(random));
    erase_secret(&s.candidate, sizeof(s.candidate));
    erase_secret(&s.verified, sizeof(s.verified));
    return saved;
}
