/* Reuse the existing UI hardware-free harness without changing its contract.
 * Compile with the same flags/sources as app_ui_test.c, replacing its test file
 * with this one. The scanner remains mocked; real debounce is checked below.
 */
#define main ui_regression_main
#define gpio_get_level baseline_gpio_get_level
#define vTaskDelayUntil baseline_vTaskDelayUntil
#include "app_ui_test.c"
#undef main
#undef gpio_get_level
#undef vTaskDelayUntil

static int64_t release_at;
int gpio_get_level(gpio_num_t pin)
{
    return pin == GPIO_NUM_5 && time_us < release_at ? 0 : 1;
}
void vTaskDelayUntil(TickType_t *wake, TickType_t ticks)
{
    *wake += ticks;
    time_us += (int64_t)ticks * 1000;
}
int main(void)
{
    release_at = 2000000;
    app_ui_init(); // center starts held: scanner retains held-at-boot ignore
    assert(!transfers);
    const int64_t short_holds[] = {0, 20000, 500000, 980000, 1000000};
    for (unsigned i = 0; i < sizeof(short_holds)/sizeof(short_holds[0]); ++i) {
        time_us = 0;
        release_at = short_holds[i];
        assert(!app_ui_confirm_wake_menu());
        assert(!app_ui_menu_open() && !transfers && !invalidates);
    }
    time_us = 0; release_at = 1000001;
    assert(app_ui_confirm_wake_menu());
    assert(time_us == 1000000 && !app_ui_menu_open() && !transfers);
    // Confirmation records only intent. Main can still perform mandatory OTA
    // checkpoint/routing before it explicitly opens the menu, with no clock.
    reset_calls();
    app_ui_open_menu();
    expect_full_menu(); // exactly one menu frame, no preceding clock transfer
    reset_calls();
    app_ui_event_t event = click(MENU_INPUT_SELECT); // close same clock mode
    assert(event.action == APP_NONE && !app_ui_menu_open());
    reset_calls();

    menu_keys_t boot_keys;
    menu_keys_init(&boot_keys, MENU_PRESSED(MENU_KEY_CENTER) | MENU_PRESSED(MENU_KEY_UP), 0);
    for (uint32_t ms = 0; ms <= 2000; ms += 20)
        assert(menu_keys_sample(&boot_keys,
            MENU_PRESSED(MENU_KEY_CENTER) | MENU_PRESSED(MENU_KEY_UP), ms) == 0);
    assert(menu_keys_sample(&boot_keys, 0, 2020) == 0);
    assert(menu_keys_sample(&boot_keys, 0, 2080) == 0); // release cannot select
    assert(menu_keys_sample(&boot_keys, MENU_PRESSED(MENU_KEY_CENTER), 2100) == 0);
    assert(menu_keys_sample(&boot_keys, MENU_PRESSED(MENU_KEY_CENTER), 2160) == 0);
    assert(menu_keys_sample(&boot_keys, MENU_PRESSED(MENU_KEY_CENTER), 3160) == MENU_INPUT_LONG);
    release_at = 0; time_us = 0;
    puts("power wake UI continuous-hold/ignore/no-clock-before-menu tests passed");
    return ui_regression_main();
}
