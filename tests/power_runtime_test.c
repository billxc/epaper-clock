/* Host-only policy/SDK-call mock. No devices, network, filesystem or retained RAM.
 * cc -std=c11 -Wall -Wextra -Werror -Itests/power_runtime_stubs \
 * -Itests/app_ui_stubs -Imain -Icomponents/axpPower -Icomponents/epaper_port \
 * tests/power_runtime_test.c main/power_runtime.c -o /tmp/power-runtime-test
 */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include "power_runtime.h"
#include "power_settings.h"
#include "axp_prot.h"
#include "rtc.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include "freertos/task.h"

static esp_sleep_wakeup_cause_t cause;
static uint64_t wake_mask, timer_us;
static power_settings_t settings = {POWER_SLEEP_PMU, 1000};
static int64_t time_us;
static int center = 1, second = 42;
static bool usb, rtc_valid = true, apply_ok = true;
static bool press_during_delay, usb_during_delay, press_during_setup;
static unsigned holds, deinits, applies, applied, alarms, shutdowns, panels, deep;
static unsigned pulls, setup_step, fail_step;
static jmp_buf asleep;

power_settings_t power_settings_get(void) { return settings; }
int64_t esp_timer_get_time(void) { return time_us; }
const char *esp_err_to_name(esp_err_t err) { (void)err; return "mock"; }
esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause(void) { return cause; }
uint64_t esp_sleep_get_ext1_wakeup_status(void) { return wake_mask; }
esp_err_t rtc_gpio_hold_dis(gpio_num_t pin) { assert(pin == 5); ++holds; return ESP_OK; }
esp_err_t rtc_gpio_deinit(gpio_num_t pin) { assert(pin == 5); ++deinits; return ESP_OK; }
static esp_err_t step(void) { return ++setup_step == fail_step ? ESP_FAIL : ESP_OK; }
esp_err_t rtc_gpio_pullup_en(gpio_num_t pin) { assert(pin == 5); ++pulls; return step(); }
esp_err_t rtc_gpio_pulldown_dis(gpio_num_t pin) { assert(pin == 5); return step(); }
esp_err_t esp_sleep_disable_ext1_wakeup_io(uint64_t mask) { assert(mask == 0); return step(); }
esp_err_t esp_sleep_enable_ext1_wakeup_io(uint64_t mask, esp_sleep_ext1_wakeup_mode_t mode)
{
    assert(mask == (UINT64_C(1) << 5) && mode == ESP_EXT1_WAKEUP_ANY_LOW);
    return step();
}
esp_err_t esp_sleep_enable_timer_wakeup(uint64_t us)
{
    timer_us = us;
    if (press_during_setup) center = 0;
    return step();
}
void esp_deep_sleep_start(void) { ++deep; longjmp(asleep, 1); }
bool axp_set_power_on_time(unsigned ms) { ++applies; applied = ms; return apply_ok; }
bool get_usb_connected(void) { return usb; }
int gpio_get_level(gpio_num_t pin) { assert(pin == 5); return center; }
void rtc_arm_next_minute(void) { ++alarms; }
void axp_pwr_off(void) { ++shutdowns; }
void EPD_Sleep(void) { ++panels; }
bool rtc_read(struct tm *now) { *now = (struct tm){.tm_sec = second}; return rtc_valid; }
void vTaskDelay(TickType_t ticks)
{
    if (ticks == portMAX_DELAY) longjmp(asleep, 2);
    assert(ticks == pdMS_TO_TICKS(40));
    if (press_during_delay) center = 0;
    if (usb_during_delay) usb = true;
}
static void reset_sleep(void)
{
    center = 1; usb = false; rtc_valid = true; second = 42;
    alarms = shutdowns = panels = deep = pulls = setup_step = fail_step = 0;
    timer_us = 0;
    press_during_delay = usb_during_delay = press_during_setup = false;
}
static void try_sleep(int expected)
{
    int result = setjmp(asleep);
    if (!result) power_runtime_sleep();
    assert(result == expected);
}
int main(void)
{
    cause = ESP_SLEEP_WAKEUP_UNDEFINED;
    assert(power_runtime_wake(false) == POWER_WAKE_MANUAL);
    assert(power_runtime_wake(true) == POWER_WAKE_SCHEDULED);
    assert(holds == 0 && deinits == 0);
    cause = ESP_SLEEP_WAKEUP_TIMER;
    assert(power_runtime_wake(false) == POWER_WAKE_SCHEDULED);
    cause = ESP_SLEEP_WAKEUP_EXT1;
    wake_mask = UINT64_C(1) << 5;
    assert(power_runtime_wake(false) == POWER_WAKE_CENTER);
    assert(power_runtime_wake(true) == POWER_WAKE_CENTER); // stale RTC flag cannot win
    wake_mask = UINT64_C(1) << 45;
    assert(power_runtime_wake(true) == POWER_WAKE_MANUAL);
    assert(holds == 4 && deinits == 4);
    cause = ESP_SLEEP_WAKEUP_OTHER;
    assert(power_runtime_wake(true) == POWER_WAKE_MANUAL);

    for (int s = 0; s < 60; ++s)
        assert(power_runtime_minute_us(true, s) == (uint64_t)(60-s) * 1000000);
    assert(power_runtime_minute_us(false, 59) == 60000000);
    assert(power_runtime_minute_us(true, -1) == 60000000);
    assert(power_runtime_minute_us(true, 60) == 60000000);

    power_runtime_apply_settings();
    assert(applies == 0); // unchanged 1s default, no periodic I2C
    const unsigned allowed[] = {128, 512, 1000, 2000};
    for (unsigned i = 0; i < 4; ++i) {
        settings.pwr_on_ms = allowed[i];
        power_runtime_apply_settings();
        assert(applied == allowed[i] && applies == i+1);
        power_runtime_apply_settings();
        assert(applies == i+1);
    }
    settings.pwr_on_ms = 512; apply_ok = false;
    power_runtime_apply_settings();
    assert(applies == 5);
    power_runtime_apply_settings();
    assert(applies == 5); // bounded owner-task retry, no busy I2C loop
    time_us = 1000000; apply_ok = true;
    power_runtime_apply_settings();
    assert(applies == 6 && applied == 512);

    reset_sleep(); try_sleep(2);
    assert(alarms == 1 && shutdowns == 1 && !panels && !deep);
    reset_sleep(); usb = true; try_sleep(0);
    assert(!alarms && !shutdowns && !deep && !panels);
    settings.sleep_mode = POWER_SLEEP_DEEP;
    reset_sleep(); try_sleep(1);
    assert(deep == 1 && panels == 1 && pulls == 1 && timer_us == 18000000);
    assert(!alarms && !shutdowns);
    reset_sleep(); second = 59; try_sleep(1); assert(timer_us == 1000000);
    reset_sleep(); second = 0; try_sleep(1); assert(timer_us == 60000000);
    reset_sleep(); rtc_valid = false; try_sleep(1); assert(timer_us == 60000000);
    reset_sleep(); center = 0; try_sleep(0); assert(!panels && !deep);
    reset_sleep(); press_during_delay = true; try_sleep(0); assert(!panels && !deep);
    reset_sleep(); usb_during_delay = true; try_sleep(0); assert(!panels && !deep);
    reset_sleep(); press_during_setup = true; try_sleep(0); assert(!deep && !shutdowns);
    for (unsigned i = 1; i <= 5; ++i) {
        reset_sleep(); fail_step = i; try_sleep(0);
        assert(!deep && !shutdowns && !alarms); // no surprise PMU fallback
    }
    puts("power runtime wake/timer/PWR/sleep policy tests passed");
}
