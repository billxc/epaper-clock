/* Real vendor class + board wrapper, mocked register bus. No hardware access.
 * g++ -std=c++17 -Wall -Wextra -Werror -Wno-error=return-type \
 * -Itests/poweroff_stubs -Itests/app_ui_stubs -Icomponents/axpPower \
 * -Icomponents/axpPower/src -Icomponents/i2c_bsp tests/poweroff_axp_test.cpp \
 * components/axpPower/axp_prot.cpp components/axpPower/src/XPowersLibInterface.cpp \
 * -o /tmp/poweroff-axp-test
 * The return-type exception is an existing vendor non-ESP host branch warning.
 */
#include <cassert>
#include <cstdio>
#include <cstring>
#include "axp_prot.h"
#include "i2c_bsp.h"

static uint8_t regs[256];
static unsigned calls, fail_call;
static bool ignore_write;
extern "C" esp_err_t i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *data, size_t len)
{
    assert(addr == 0x34 && len == 1);
    if (++calls == fail_call) return ESP_FAIL;
    *data = regs[reg];
    return ESP_OK;
}
extern "C" esp_err_t i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t *data, size_t len)
{
    assert(addr == 0x34 && len == 1);
    if (++calls == fail_call) return ESP_FAIL;
    if (!ignore_write) {
        if (reg == 0x49) regs[reg] &= ~*data;
        else regs[reg] = *data;
    }
    return ESP_OK;
}
static void reset(void)
{
    memset(regs, 0xa5, sizeof(regs));
    calls = fail_call = 0;
    ignore_write = false;
}
int main(void)
{
    reset();
    assert(axp_configure_power_key_fallback());
    assert(regs[0x22] == 0xa6); // enable OFFLEVEL, poweroff not restart
    assert(regs[0x27] == 0xad); // 10s, preserve IRQ and power-on bits
    for (unsigned n = 1; n <= 6; ++n) {
        reset(); fail_call = n;
        assert(!axp_configure_power_key_fallback());
    }
    reset(); ignore_write = true; assert(!axp_configure_power_key_fallback());
    reset(); regs[0x49] = 0xff;
    assert(axp_power_key_begin());
    assert(regs[0x41] == 0xaf && regs[0x49] == 0xf0);
    uint8_t events;
    reset(); regs[0x49] = 0xf6;
    assert(axp_power_key_read(&events) && events == 6 && regs[0x49] == 0xf0);
    assert(axp_power_key_read(&events) && !events && regs[0x49] == 0xf0);
    for (unsigned n = 1; n <= 2; ++n) {
        reset(); fail_call = n; events = 0xff;
        assert(!axp_power_key_read(&events) && !events);
    }
    reset(); regs[0x10] = 0x80;
    assert(axp_power_off_checked() && regs[0x10] == 0x81);
    for (unsigned n = 1; n <= 2; ++n) {
        reset(); fail_call = n; assert(!axp_power_off_checked());
    }
    puts("AXP production wrapper: 10s always-on fallback/readback, narrow IRQ W1C, I2C failures passed");
}
