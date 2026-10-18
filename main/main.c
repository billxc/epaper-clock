#include <assert.h>
#include "sdkconfig.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
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
    i2c_master_init();
    axp_init();
    rtc_init();
    bool force_full = false;
    if (get_usb_connected()) force_full = usb_set_time();
    i2c_shtc3_init();

    esp_err_t mounted = cache_mount();
    if (mounted != ESP_OK)
        ESP_LOGW("clock", "No frame cache (%s): full refresh fallback", esp_err_to_name(mounted));
    uint8_t *frame = heap_caps_malloc(CLOCK_FRAME_SIZE, MALLOC_CAP_SPIRAM);
    assert(frame);
    epaper_port_init();

    for (;;) {
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
        // With USB attached, keep the same firmware running rather than repeatedly
        // requesting PMU shutdown on an externally powered board.
        vTaskDelay(pdMS_TO_TICKS((60 - after.tm_sec) * 1000));
        started = esp_timer_get_time();
    }
}
