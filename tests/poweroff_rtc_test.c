/* Production RTC cancel path with a register mock; no real I2C or RTC.
 * gcc -D_POSIX_C_SOURCE=200809L -std=c11 -Wall -Wextra -Werror \
 * -Itests/poweroff_stubs -Itests/app_ui_stubs -Imain -Icomponents/i2c_bsp \
 * tests/poweroff_rtc_test.c main/rtc.c -o /tmp/poweroff-rtc-test
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "rtc.h"
#include "i2c_bsp.h"
#include "driver/gpio.h"
#include "freertos/semphr.h"
static uint8_t regs[256];
static unsigned calls, fail_call, locks;
static bool ignore_write;
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage) { return storage; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait)
{
    assert(s && wait == portMAX_DELAY && !locks); ++locks; return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { assert(s && locks == 1); --locks; return pdTRUE; }
esp_err_t gpio_config(const gpio_config_t *c) { (void)c; return ESP_OK; }
esp_err_t i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *data, size_t len)
{
    assert(addr == 0x51);
    if (++calls == fail_call) return ESP_FAIL;
    memcpy(data, regs + reg, len); return ESP_OK;
}
esp_err_t i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t *data, size_t len)
{
    assert(addr == 0x51);
    if (++calls == fail_call) return ESP_FAIL;
    if (!ignore_write) memcpy(regs + reg, data, len);
    return ESP_OK;
}
static void reset(void)
{
    memset(regs, 0xa5, sizeof(regs)); regs[1] = 0xff;
    calls = fail_call = 0; ignore_write = false;
}
int main(void)
{
    clock_rtc_init(); reset();
    assert(rtc_cancel_alarm() && !locks);
    assert(regs[1] == 0x3f); // Preserve unrelated control bits, clear AIE+AF.
    for (unsigned i = 0x0b; i <= 0x0f; ++i) assert(regs[i] == 0x80);
    assert(regs[0x04] == 0xa5 && regs[0x10] == 0xa5); // Calendar untouched.
    for (unsigned n = 1; n <= 4; ++n) {
        reset(); fail_call = n; assert(!rtc_cancel_alarm() && !locks);
    }
    reset(); ignore_write = true; assert(!rtc_cancel_alarm() && !locks);
    puts("RTC production cancellation: AIE/AF/matches cleared, no calendar change, read/write failures passed");
}
