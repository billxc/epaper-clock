#include <assert.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "i2c_bsp.h"
#include "axp_prot.h"
#include "shtc3_bsp.h"
#include "sdcard_bsp.h"
#include "epaper_port.h"
#include "display.h"
#include "rtc.h"
#include "network.h"
#include "app_runtime.h"
#include "admin.h"
#include "provision.h"
#include "ota.h"
#include "usb_time.h"

static atomic_int pending_action;
static int64_t last_button_activity;
static bool clock_dirty, setup_complete;

static void remember_clock_choice(void)
{
    if (setup_complete) return;
    nvs_handle_t handle;
    esp_err_t result = nvs_open("clock_app", NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_set_u8(handle, "ready", 1);
        if (result == ESP_OK) result = nvs_commit(handle);
        nvs_close(handle);
    }
    if (result == ESP_OK) setup_complete = true;
    else ESP_LOGW("clock", "Unable to retain clock-mode choice: %s", esp_err_to_name(result));
}

static uint8_t *clock_frame, *previous_frame;

app_action_t app_poll(bool *activity, bool *selected)
{
    if (activity) *activity = false;
    if (selected) *selected = false;
    if (app_ui_owner()) {
        app_ui_event_t event = app_ui_poll();
        if (activity) *activity = event.activity;
        if (selected) *selected = event.select;
        if (event.activity) {
            last_button_activity = esp_timer_get_time();
            admin_note_activity();
        }
        if (event.action != APP_NONE) {
            atomic_store(&pending_action, event.action);
            network_cancel();
        }
        if (admin_idle_expired() && atomic_load(&pending_action) == APP_NONE) {
            atomic_store(&pending_action, APP_CLOCK);
            network_cancel();
        }
        if (usb_time_poll()) clock_dirty = true;
    }
    return (app_action_t)atomic_load(&pending_action);
}

static bool network_interrupted(void)
{
    return app_poll(NULL, NULL) != APP_NONE;
}

static app_action_t chosen_or(app_action_t fallback)
{
    app_action_t chosen = (app_action_t)atomic_load(&pending_action);
    return chosen == APP_NONE ? fallback : chosen;
}

static app_action_t show_result(const char *title, const char *const *lines,
                                size_t count, bool return_to_menu)
{
    app_ui_message(title, lines, count);
    int64_t until = esp_timer_get_time() + 2000000;
    for (;;) {
        bool selected;
        app_action_t action = app_poll(NULL, &selected);
        if (action != APP_NONE) return action;
        if (return_to_menu && selected) app_ui_open_menu();
        if (!return_to_menu && esp_timer_get_time() >= until) return APP_CLOCK;
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

static app_action_t diagnose(void)
{
    const char *start[] = {"正在连接已保存的 Wi-Fi", "长按中键可以打开菜单"};
    app_ui_message("本机自动调试", start, 2);
    esp_err_t wifi = network_connect();
    char ip[16] = "", ssid_line[80], ip_line[48], wifi_line[64], ntp_line[64], time_line[64];
    clock_wifi_config_t config;
    network_get_config(&config);
    snprintf(ssid_line, sizeof(ssid_line), "SSID: %s", config.ssid[0] ? config.ssid : "未配置");
    network_station_ip(ip, sizeof(ip));
    snprintf(ip_line, sizeof(ip_line), "IP: %s", ip[0] ? ip : "未取得");
    snprintf(wifi_line, sizeof(wifi_line), "Wi-Fi: %s", wifi == ESP_OK ? "连接成功" : esp_err_to_name(wifi));
    time_t epoch = 0;
    esp_err_t ntp = wifi == ESP_OK ? network_query_time(&epoch) : ESP_ERR_INVALID_STATE;
    snprintf(ntp_line, sizeof(ntp_line), "NTP: %s", wifi != ESP_OK ? "未执行" : ntp == ESP_OK ? "响应成功" : esp_err_to_name(ntp));
    if (ntp == ESP_OK) {
        struct tm time;
        localtime_r(&epoch, &time);
        strftime(time_line, sizeof(time_line), "%Y-%m-%d %H:%M:%S", &time);
    } else {
        snprintf(time_line, sizeof(time_line), "未取得 NTP 时间响应");
    }
    network_stop();
    if (chosen_or(APP_NONE) != APP_NONE) return chosen_or(APP_CLOCK);
    const char *results[] = {wifi_line, ssid_line, ip_line, ntp_line, time_line, "仅检测，不修改 RTC 或配置", "中键返回选单"};
    return show_result("本机自动调试结果", results, 7, true);
}

static app_action_t synchronize(void)
{
    const char *start[] = {"正在连接 NTP 并校准时钟", "长按中键可以打开菜单"};
    app_ui_message("立刻自动对时", start, 2);
    esp_err_t result = network_sync_now();
    network_stop();
    if (chosen_or(APP_NONE) != APP_NONE) return chosen_or(APP_CLOCK);
    const char *lines[] = {
        result == ESP_OK ? "对时成功，已更新 RTC" : "对时失败，保留原 RTC 时间",
        result == ESP_OK ? "即将回到时钟模式" : esp_err_to_name(result),
    };
    return show_result("立刻自动对时", lines, 2, false);
}

static app_action_t clock_mode(bool manual_start)
{
    esp_err_t mounted = cache_mount();
    if (mounted != ESP_OK)
        ESP_LOGW("clock", "SD cache unavailable: %s", esp_err_to_name(mounted));
    bool first = true;
    int last_minute = -1;
    int64_t awake_until = 0;
    for (;;) {
        app_action_t action = app_poll(NULL, NULL);
        if (action != APP_NONE) {
            cache_unmount();
            return action;
        }
        struct tm now;
        bool valid = rtc_read(&now);
        if (first || now.tm_min != last_minute || clock_dirty) {
            bool synced = network_daily_sync();
            if (chosen_or(APP_NONE) != APP_NONE) {
                cache_unmount();
                return chosen_or(APP_CLOCK);
            }
            valid = rtc_read(&now);
            bool full = (first && manual_start) || synced || clock_dirty || !valid || now.tm_min == 0 || now.tm_min == 30;
            bool cached = cache_read(previous_frame, CLOCK_FRAME_SIZE);
            float temperature = NAN, humidity = NAN;
            if (SHTC3_GetEnvTemperatureHumidity(&temperature, &humidity) != 0)
                temperature = humidity = NAN;
            clock_render(clock_frame, &now, valid, temperature, humidity, get_battery_power());
            app_ui_present_clock(cached ? previous_frame : NULL, clock_frame, full || !cached);
            if (!cache_write(clock_frame, CLOCK_FRAME_SIZE))
                ESP_LOGW("clock", "Unable to persist display cache");
            last_minute = now.tm_min;
            clock_dirty = false;
            // Give a manual boot five visible seconds AFTER its first refresh,
            // not five seconds partly consumed by a slow e-paper update.
            if (first && manual_start) awake_until = esp_timer_get_time() + 5000000;
            first = false;
            // Cross-minute refreshes are caught by the next iteration.
            continue;
        }
        int64_t recent = last_button_activity + 5000000;
        if (last_button_activity > 0 && recent > awake_until) awake_until = recent;
        if (!get_usb_connected() && !app_ui_menu_open() && esp_timer_get_time() >= awake_until) {
            cache_unmount();
            network_stop();
            rtc_arm_next_minute();
            axp_pwr_off();
            vTaskDelay(portMAX_DELAY);
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

void app_main(void)
{
    setenv("TZ", CONFIG_CLOCK_TIMEZONE, 1);
    tzset();
    ESP_ERROR_CHECK(nvs_flash_init());
    i2c_master_init();
    axp_init();
    bool scheduled_start = rtc_alarm_pending();
    clock_rtc_init();
    epaper_port_init();
    app_ui_init();
    bool force_initial = gpio_get_level(GPIO_NUM_4) == 0;
    network_init();
    nvs_handle_t state;
    uint8_t ready = 0;
    if (nvs_open("clock_app", NVS_READONLY, &state) == ESP_OK) {
        nvs_get_u8(state, "ready", &ready);
        nvs_close(state);
    }
    setup_complete = ready != 0;
    network_set_poll_hook(network_interrupted);
    usb_time_init(get_usb_connected());
    i2c_shtc3_init();
    clock_frame = heap_caps_malloc(CLOCK_FRAME_SIZE, MALLOC_CAP_SPIRAM);
    previous_frame = heap_caps_malloc(CLOCK_FRAME_SIZE, MALLOC_CAP_SPIRAM);
    assert(clock_frame && previous_frame);
    struct tm initial_time;
    bool valid_time = rtc_read(&initial_time);
    // A pending image must reach a bounded local checkpoint before becoming
    // valid. A display-busy hang causes a watchdog reset and SDK rollback.
    // This runs only on the first OTA boot, never every minute.
    esp_ota_img_states_t boot_state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &boot_state) == ESP_OK &&
        boot_state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
        const char *checks[] = {"RTC 与显示初始化检查", "通过后确认新版本", "不要求网络、NTP 或 SD 可用"};
        app_ui_message("新固件启动验证", checks, 3);
        ESP_ERROR_CHECK(esp_task_wdt_delete(NULL));
    }
    // Confirm before any minute-cycle PMU shutdown. An unset RTC calendar,
    // absent optional SD and offline network are not boot-health failures.
    if (!ota_boot_check(true)) {
        const char *failure[] = {"无法确认新固件或回滚", "已停止正常时钟关机流程", "请连接 USB 恢复固件"};
        app_ui_message("升级恢复失败", failure, 3);
        vTaskDelay(portMAX_DELAY);
        return;
    }
    if (force_initial) ota_management_finished();
    app_action_t action = force_initial ? APP_INITIALIZE :
        ota_resume_management() ? APP_LAN :
        (!network_configured() && !setup_complete) || (!valid_time && !scheduled_start && !setup_complete) ? APP_INITIALIZE : APP_CLOCK;
    bool manual_start = !scheduled_start;
    for (;;) {
        atomic_store(&pending_action, APP_NONE);
        network_clear_cancel();
        app_ui_set_mode(action);
        switch (action) {
        case APP_INITIALIZE:
        case APP_BLE: {
            bool initial = action == APP_INITIALIZE;
            provision_result_t result = provision_run(initial);
            action = result.next == APP_NONE ? APP_CLOCK : result.next;
            if (initial && action == APP_CLOCK && result.wifi_saved && !result.time_set) {
                network_clear_cancel();
                const char *syncing[] = {"Wi-Fi 配置已保存，正在首次校时", "若 NTP 不可用，可用蓝牙手动设时"};
                app_ui_message("初始化", syncing, 2);
                esp_err_t sync = network_sync_now();
                network_stop();
                if (chosen_or(APP_NONE) != APP_NONE) action = chosen_or(APP_CLOCK);
                else if (sync != ESP_OK && !rtc_read(&initial_time)) {
                    const char *lines[] = {"首次 NTP 校时失败", "请用蓝牙管理手动设置时间", "仍留在初始化模式"};
                    show_result("尚未取得有效时间", lines, 3, false);
                    action = chosen_or(APP_INITIALIZE);
                }
            }
            manual_start = true;
            break;
        }
        case APP_LAN:
            action = admin_run();
            manual_start = true;
            break;
        case APP_DIAGNOSTICS:
            action = diagnose();
            manual_start = true;
            break;
        case APP_SYNC:
            action = synchronize();
            manual_start = true;
            break;
        case APP_CLOCK:
        default:
            // One explicit/finished setup choice persists across PMU cold boots,
            // including an intentionally offline clock. Never write each minute.
            remember_clock_choice();
            action = clock_mode(manual_start);
            manual_start = true;
            break;
        }
    }
}
