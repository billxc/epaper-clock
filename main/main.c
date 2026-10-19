#include <assert.h>
#include "sdkconfig.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "i2c_bsp.h"
#include "axp_prot.h"
#include "shtc3_bsp.h"
#include "epaper_port.h"
#include "sdcard_bsp.h"
#include "display.h"
#include "rtc.h"
#include "network.h"
#include "admin.h"
#include "provision.h"

#define MANAGEMENT_BUTTON GPIO_NUM_5 // Dial center, active low.
#define PROVISION_BUTTON GPIO_NUM_4  // Dial up: request BLE reprovisioning.

static bool management_button_pressed(void)
{
    static bool held;
    bool down = gpio_get_level(MANAGEMENT_BUTTON) == 0;
    bool pressed = down && !held;
    held = down;
    return pressed;
}

static bool provision_button_pressed(void)
{
    static bool held;
    bool down = gpio_get_level(PROVISION_BUTTON) == 0;
    bool pressed = down && !held;
    held = down;
    return pressed;
}

static void run_management(bool reprovision)
{
    clock_wifi_config_t config;
    network_get_config(&config);
    if (reprovision || !config.ssid[0]) {
        if (!provision_run()) return;
        esp_err_t result = network_sync_now();
        ESP_LOGW("clock", "Provisioned Wi-Fi sync: %s", esp_err_to_name(result));
    }
    admin_run();
}

// Only USB-powered starts wait for a host; battery starts never wait here.
static bool usb_set_time(void)
{
    usb_serial_jtag_driver_config_t config = {.rx_buffer_size = 256, .tx_buffer_size = 256};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&config));
    const char ready[] = "CLOCK_READY: send TIME <unix-seconds>\n";
    usb_serial_jtag_write_bytes(ready, sizeof(ready) - 1, pdMS_TO_TICKS(100));
    int64_t deadline = esp_timer_get_time() + 10000000;
    char line[40];
    size_t used = 0;
    bool changed = false;
    while (esp_timer_get_time() < deadline) {
        char c;
        if (usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(100)) != 1) continue;
        if (c == '\r') continue;
        if (c == '\n') {
            line[used] = '\0';
            int64_t epoch;
            if (sscanf(line, "TIME %" SCNd64, &epoch) == 1 &&
                epoch >= 1704067200LL && epoch <= 4102444799LL) {
                rtc_set((time_t)epoch);
                const char ok[] = "TIME_OK\n";
                usb_serial_jtag_write_bytes(ok, sizeof(ok) - 1, pdMS_TO_TICKS(100));
                usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(100));
                changed = true;
                break;
            }
            used = 0;
        } else if (used < sizeof(line) - 1) {
            line[used++] = c;
        }
    }
    ESP_ERROR_CHECK(usb_serial_jtag_driver_uninstall());
    return changed;
}

void app_main(void)
{
    int64_t started = esp_timer_get_time();
    setenv("TZ", CONFIG_CLOCK_TIMEZONE, 1);
    tzset();
    ESP_ERROR_CHECK(nvs_flash_init());
    i2c_master_init();
    axp_init();
    bool scheduled_start = rtc_alarm_pending();
    clock_rtc_init();
    gpio_config_t button = {
        .pin_bit_mask = (1ULL << MANAGEMENT_BUTTON) | (1ULL << PROVISION_BUTTON),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button));
    network_init();
    bool reprovision = provision_button_pressed();
    bool enter_management = !scheduled_start || management_button_pressed() || reprovision;
    bool force_full = false;
    if (get_usb_connected()) force_full = usb_set_time();
    reprovision |= provision_button_pressed();
    enter_management |= management_button_pressed() || reprovision;
    i2c_shtc3_init();

    esp_err_t mounted = cache_mount();
    if (mounted != ESP_OK)
        ESP_LOGW("clock", "No frame cache (%s): full refresh fallback", esp_err_to_name(mounted));
    uint8_t *frame = heap_caps_malloc(CLOCK_FRAME_SIZE, MALLOC_CAP_SPIRAM);
    assert(frame);
    epaper_port_init();
    if (enter_management) {
        run_management(reprovision);
        force_full = true;
    }

    for (;;) {
        bool reconfigure = provision_button_pressed();
        if (management_button_pressed() || reconfigure) {
            run_management(reconfigure);
            force_full = true;
        }
        if (network_daily_sync()) force_full = true;
        struct tm now;
        bool valid = rtc_read(&now);
        // Preserve the vendor's old-frame restore + new-frame partial refresh.
        bool partial = cache_read(frame, CLOCK_FRAME_SIZE) && valid &&
                       !force_full && now.tm_min != 0 && now.tm_min != 30;
        EPD_Init();
        if (partial) EPD_Display_Partial(frame, 0, 0, EPD_WIDTH, EPD_HEIGHT);

        float temperature = NAN, humidity = NAN;
        if (SHTC3_GetEnvTemperatureHumidity(&temperature, &humidity) != 0)
            temperature = humidity = NAN;
        clock_render(frame, &now, valid, temperature, humidity, get_battery_power());
        if (partial) EPD_Display_Partial(frame, 0, 0, EPD_WIDTH, EPD_HEIGHT);
        else EPD_Display_Base(frame);
        EPD_Sleep();
        if (!cache_write(frame, CLOCK_FRAME_SIZE))
            ESP_LOGW("clock", "Cache not saved: next update requires full refresh");

        // WARN is intentional: visible with the low-volume production log level.
        ESP_LOGW("clock", "Update finished in %" PRId64 " ms", (esp_timer_get_time() - started) / 1000);
        force_full = false;
        struct tm after;
        bool after_valid = rtc_read(&after);
        // A startup/full refresh can cross an exact minute boundary. Catch up
        // before arming the following minute, rather than skipping this minute.
        if (valid && after_valid &&
            (after.tm_min != now.tm_min || after.tm_hour != now.tm_hour)) {
            started = esp_timer_get_time();
            continue;
        }
        if (!get_usb_connected()) {
            cache_unmount();
            rtc_arm_next_minute();
            axp_pwr_off();
            // shutdown should cut power; never spin if hardware does not comply.
            vTaskDelay(portMAX_DELAY);
        }
        // Only USB mode stays awake and polls the dial key; battery mode has
        // already powered off. PWR manually starts management from power-off.
        int64_t deadline = esp_timer_get_time() + (60 - after.tm_sec) * 1000000LL;
        while (esp_timer_get_time() < deadline) {
            bool reconfigure = provision_button_pressed();
            if (management_button_pressed() || reconfigure) {
                run_management(reconfigure);
                force_full = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        started = esp_timer_get_time();
    }
}
