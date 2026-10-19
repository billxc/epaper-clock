#include "usb_time.h"
#include "rtc.h"
#include <inttypes.h>
#include <stdio.h>
#include <time.h>
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"

static bool enabled;
static char line[40];
static size_t used;

void usb_time_init(bool connected)
{
    if (!connected) return;
    usb_serial_jtag_driver_config_t config = {.rx_buffer_size = 256, .tx_buffer_size = 256};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&config));
    enabled = true;
    const char ready[] = "CLOCK_READY: send TIME <unix-seconds>\n";
    usb_serial_jtag_write_bytes(ready, sizeof(ready) - 1, 0);
}

bool usb_time_poll(void)
{
    if (!enabled) return false;
    char buffer[64];
    int received = usb_serial_jtag_read_bytes(buffer, sizeof(buffer), 0);
    bool changed = false;
    for (int i = 0; i < received; ++i) {
        char c = buffer[i];
        if (c == '\r') continue;
        if (c == '\n') {
            line[used] = '\0';
            int64_t epoch;
            if (sscanf(line, "TIME %" SCNd64, &epoch) == 1 && epoch >= 946684800LL && epoch <= 4102444799LL) {
                struct tm local;
                time_t value = (time_t)epoch;
                localtime_r(&value, &local);
                if (local.tm_year >= 100 && local.tm_year <= 199) {
                    rtc_set(value);
                    const char ok[] = "TIME_OK\n";
                    usb_serial_jtag_write_bytes(ok, sizeof(ok) - 1, pdMS_TO_TICKS(100));
                    changed = true;
                }
            }
            used = 0;
        } else if (used < sizeof(line) - 1) {
            line[used++] = c;
        }
    }
    return changed;
}
