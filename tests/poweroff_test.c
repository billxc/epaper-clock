/* Host-only: real PWR request core + UI integration, stub I2C/EPD/OTA/SDK.
 * gcc -std=c11 -Wall -Wextra -Werror -Itests/poweroff_stubs \
 * -Itests/power_runtime_stubs -Itests/app_ui_stubs -Imain \
 * -Icomponents/axpPower -Icomponents/epaper_port tests/poweroff_test.c \
 * main/poweroff.c main/app_ui.c main/menu_model.c main/render_mode.c -o /tmp/poweroff-test
 * NOT a PMU electrical, panel timing, real TF, OTA transport or BUSY test.
 */
#define main ui_regression_main
#define poweroff_sample unused_ui_poweroff_sample
#include "app_ui_test.c"
#undef main
#undef poweroff_sample
#include "poweroff.h"
#include "axp_prot.h"
#include "esp_sleep.h"
#include "freertos/task.h"

static uint8_t irq;
static bool read_ok = true, begin_ok = true, alarm_ok = true, wake_ok = true;
static bool off_ok = true, busy;
static unsigned cancelled_alarm, cancelled_wake, off_commands, fallback, delay_count;
static unsigned expected_transfers;

bool axp_power_key_begin(void) { irq = 0; return begin_ok; }
bool axp_power_key_read(uint8_t *events)
{
    *events = irq;
    irq = 0;
    return read_ok;
}
bool axp_configure_power_key_fallback(void) { ++fallback; return true; }
bool ota_busy(void) { return busy; }
bool rtc_cancel_alarm(void) { ++cancelled_alarm; return alarm_ok; }
esp_err_t esp_sleep_disable_wakeup_source(esp_sleep_wakeup_cause_t source)
{
    assert(source == ESP_SLEEP_WAKEUP_ALL && cancelled_alarm);
    ++cancelled_wake;
    return wake_ok ? ESP_OK : ESP_FAIL;
}
bool axp_power_off_checked(void)
{
    assert(cancelled_alarm && cancelled_wake);
    assert(transfers == expected_transfers && sleeps == 4);
    assert(!app_ui_menu_open() && !app_ui_clock_visible());
    for (size_t i = 0; i < CLOCK_FRAME_SIZE; ++i) assert(frames[transfers - 1][i] == 0xff);
    ++off_commands;
    return off_ok;
}
void vTaskDelay(TickType_t ticks)
{
    assert(ticks == pdMS_TO_TICKS(1500));
    assert(off_commands);
    ++delay_count;
}
static void sample(unsigned ms, uint8_t events)
{
    time_us += (int64_t)ms * 1000;
    irq = events;
    poweroff_sample();
}
static void start(void)
{
    read_ok = begin_ok = true;
    assert(poweroff_init());
    assert(!poweroff_tracking() && !poweroff_requested(false));
}
static void hold(unsigned ms)
{
    for (unsigned elapsed = 0; elapsed < ms; elapsed += 20) sample(20, 0);
}
static void request(void)
{
    start(); sample(20, 2); hold(4000);
    assert(!poweroff_requested(false) && poweroff_tracking()); // must release
    sample(20, 1);
    assert(poweroff_requested(false));
}
static void reset_shutdown(void)
{
    reset_calls();
    cancelled_alarm = cancelled_wake = off_commands = fallback = delay_count = 0;
    alarm_ok = wake_ok = off_ok = true;
    busy = false;
}
static void verify_fade(bool mono)
{
    assert(transfers == (mono ? 7 : 4));
    assert(kinds[0] == (mono ? 'B' : 'G'));
    for (unsigned step = 1; step <= 3; ++step) {
        unsigned n = mono ? step * 2 : step;
        assert(kinds[n] == (mono ? 'P' : 'G'));
        for (size_t pixel = 0; pixel < (size_t)EPD_WIDTH * EPD_HEIGHT; ++pixel) {
            unsigned shift = 6 - 2 * (pixel % 4);
            unsigned original = (frames[0][pixel / 4] >> shift) & 3;
            unsigned actual = (frames[n][pixel / 4] >> shift) & 3;
            assert(actual == (pixel % 3 < step ? 3 : original));
        }
    }
}
int main(void)
{
    app_ui_init();
    // Startup residue including the power-on long press must not re-shutdown.
    irq = 15; start(); sample(20, 4); hold(5000); sample(20, 1);
    assert(!poweroff_requested(false));
    // The FIRST fresh press works; no sacrificial short click needed.
    sample(20, 2); hold(4000); sample(20, 1);
    assert(poweroff_requested(false));
    start(); sample(20, 2); hold(500); sample(20, 9); hold(5000);
    assert(!poweroff_tracking() && !poweroff_requested(false));
    start(); sample(20, 2); hold(3980); sample(20, 1);
    assert(!poweroff_requested(false));
    // Read/ack failure and long polling gap cannot synthesize a hold.
    start(); sample(20, 2); hold(2000); read_ok = false; sample(20, 0);
    read_ok = true; hold(5000); sample(20, 1); assert(!poweroff_requested(false));
    start(); sample(20, 2); hold(1000); sample(500, 0); hold(4000);
    sample(20, 1); assert(!poweroff_requested(false));
    start(); sample(20, 3); hold(5000); assert(!poweroff_requested(false));
    begin_ok = false; assert(!poweroff_init()); sample(20, 2); hold(5000);
    assert(!poweroff_requested(false));

    request(); busy = true;
    assert(!poweroff_requested(true));
    assert(!poweroff_execute()); // no display or wake/PMU writes while OTA owns
    assert(!off_commands && !transfers && !cancelled_alarm);
    busy = false; assert(poweroff_requested(false)); // delayed request retained
    for (render_mode_t mode = RENDER_MONO; mode <= RENDER_INK_MATCH; ++mode) {
        request(); reset_shutdown(); render_mode_set(mode);
        app_ui_open_menu(); assert(app_ui_menu_open()); reset_calls();
        expected_transfers = mode == RENDER_MONO ? 7 : 4;
        assert(!poweroff_execute()); // mock CPU remains alive -> bounded recovery
        verify_fade(mode == RENDER_MONO);
        assert(render_mode_get() == mode); // no preference change on success
        assert(off_commands == 1 && delay_count == 1 && fallback == 1);
        assert(invalidates == (mode == RENDER_MONO ? 5 : 1));
    }
    request(); reset_shutdown(); alarm_ok = false;
    assert(!poweroff_execute() && !off_commands && !transfers && fallback == 1);
    request(); reset_shutdown(); wake_ok = false;
    assert(!poweroff_execute() && !off_commands && !transfers && fallback == 1);
    request(); reset_shutdown(); render_mode_set(RENDER_MONO); fail_init = true;
    assert(!poweroff_execute() && !off_commands && !transfers && fallback == 1);
    for (unsigned failure = 1; failure <= 7; ++failure) {
        request(); reset_shutdown(); fail_transfer = failure;
        assert(!poweroff_execute() && !off_commands && transfers == failure);
        assert(sleeps >= 1 && fallback == 1);
    }
    request(); reset_shutdown(); expected_transfers = 7; off_ok = false;
    assert(!poweroff_execute() && off_commands == 1 && !delay_count && fallback == 1);
    for (unsigned i = 0; i < allocated; ++i) free(allocations[i]);
    puts("poweroff host: fresh-edge/4s/release, OTA defer, prompt/fade/white/sleep/off, wake cancellation and failures passed");
    return 0;
}
