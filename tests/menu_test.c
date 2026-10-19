/* Host CI, no ESP-IDF required:
 * cc -std=c11 -Wall -Wextra -Werror -I main tests/menu_test.c main/menu_model.c -o menu_test
 * ./menu_test
 * Local implementation environment has no C compiler; run in CI/on a host.
 */
#include "menu_model.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static app_ui_event_t input(menu_model_t *m, menu_input_t key)
{
    app_ui_event_t e = menu_model_input(m, key);
    assert(e.menu_open == m->open);
    assert(e.activity);
    return e;
}

static void choose_index(menu_model_t *m, unsigned index)
{
    menu_model_open(m);
    for (unsigned n = 0; m->selected != index; ++n) {
        assert(n < MENU_ITEM_COUNT);
        app_ui_event_t e = input(m, MENU_INPUT_DOWN);
        assert(e.action == APP_NONE && !e.select && e.menu_open);
    }
}

static void test_navigation(void)
{
    menu_model_t m;
    menu_model_init(&m, APP_CLOCK);
    assert(!m.open && m.selected == 0);
    app_ui_event_t e = input(&m, MENU_INPUT_UP);
    assert(!m.open && !e.select && e.action == APP_NONE && m.selected == 0);
    e = input(&m, MENU_INPUT_DOWN);
    assert(!m.open && !e.select && e.action == APP_NONE);
    e = input(&m, MENU_INPUT_SELECT);
    assert(e.select && !e.menu_open && e.action == APP_NONE);
    e = input(&m, MENU_INPUT_LONG);
    assert(m.open && !m.confirming && !e.select && e.action == APP_NONE);
    input(&m, MENU_INPUT_UP);
    assert(m.selected == MENU_ITEM_COUNT - 1);
    input(&m, MENU_INPUT_DOWN);
    assert(m.selected == 0);
    for (unsigned i = 0; i < MENU_ITEM_COUNT; ++i) input(&m, MENU_INPUT_DOWN);
    assert(m.selected == 0);
    input(&m, MENU_INPUT_DOWN);
    input(&m, MENU_INPUT_LONG);
    assert(m.selected == 1);  /* Long press while open does not select. */
    menu_model_set_mode(&m, APP_BLE);
    assert(m.open && m.selected == 1 && m.mode == APP_BLE);
    menu_model_open(&m);
    assert(m.selected == 2 && !m.confirming);
    assert(menu_model_item(MENU_ITEM_COUNT) == APP_NONE);
    assert(strcmp(menu_model_label(MENU_ITEM_COUNT), "") == 0);
    for (unsigned i = 0; i < MENU_ITEM_COUNT; ++i) assert(strlen(menu_model_label(i)) > 0);
    e = menu_model_input(&m, (menu_input_t)0);
    assert(!e.activity && !e.select && e.action == APP_NONE);
}

static void test_single_select_and_same_mode(void)
{
    const app_action_t modes[] = {APP_CLOCK, APP_LAN, APP_BLE, APP_INITIALIZE};
    for (unsigned mode = 0; mode < sizeof(modes) / sizeof(modes[0]); ++mode) {
        for (unsigned i = 0; i < MENU_ITEM_COUNT - 1; ++i) {
            menu_model_t m;
            menu_model_init(&m, modes[mode]);
            choose_index(&m, i);
            app_ui_event_t e = input(&m, MENU_INPUT_SELECT);
            app_action_t wanted = menu_model_item(i);
            assert(!m.open && !m.confirming && !e.select);
            assert(e.action == (wanted == modes[mode] ? APP_NONE : wanted));
            assert(m.mode == modes[mode]); /* Supervisor applies mode, not UI. */
        }
    }
    const app_action_t tasks[] = {APP_DIAGNOSTICS, APP_SYNC};
    for (unsigned i = 0; i < 2; ++i) {
        menu_model_t m;
        menu_model_init(&m, tasks[i]);
        menu_model_open(&m);
        app_ui_event_t e = input(&m, MENU_INPUT_SELECT);
        assert(e.action == tasks[i] && !m.open); /* Tasks remain repeatable. */
    }
}

static void test_initialization_confirmation(void)
{
    menu_model_t m;
    menu_model_init(&m, APP_CLOCK);
    choose_index(&m, MENU_ITEM_COUNT - 1);
    app_ui_event_t e = input(&m, MENU_INPUT_SELECT);
    assert(m.open && m.confirming && !m.confirm_yes);
    assert(e.action == APP_NONE && !e.select);
    e = input(&m, MENU_INPUT_SELECT); /* Default is CANCEL. */
    assert(m.open && !m.confirming && !m.confirm_yes && e.action == APP_NONE);
    assert(m.selected == MENU_ITEM_COUNT - 1);
    input(&m, MENU_INPUT_SELECT);
    input(&m, MENU_INPUT_UP);
    assert(m.confirm_yes);
    input(&m, MENU_INPUT_DOWN);
    assert(!m.confirm_yes);
    input(&m, MENU_INPUT_DOWN);
    e = input(&m, MENU_INPUT_SELECT);
    assert(!m.open && !m.confirming && !m.confirm_yes);
    assert(e.action == APP_INITIALIZE && !e.select);
    menu_model_set_mode(&m, APP_INITIALIZE);
    menu_model_open(&m);
    input(&m, MENU_INPUT_SELECT);
    assert(m.confirming && !m.confirm_yes);
    input(&m, MENU_INPUT_DOWN);
    e = input(&m, MENU_INPUT_SELECT);
    assert(!m.open && e.action == APP_NONE); /* Same mode never restarts. */
    menu_model_open(&m);
    input(&m, MENU_INPUT_SELECT);
    input(&m, MENU_INPUT_DOWN);
    menu_model_open(&m); /* Explicit result-page return resets confirmation. */
    assert(m.open && !m.confirming && !m.confirm_yes);
}

static void test_background_completion_preserves_menu(void)
{
    const app_action_t tasks[] = {APP_DIAGNOSTICS, APP_SYNC, APP_LAN};
    for (unsigned i = 0; i < sizeof(tasks) / sizeof(tasks[0]); ++i) {
        menu_model_t m;
        menu_model_init(&m, tasks[i]);
        choose_index(&m, 2);
        menu_model_set_mode(&m, APP_CLOCK); /* Network/NTP task completes. */
        assert(m.open && m.selected == 2 && m.mode == APP_CLOCK);
        assert(!m.confirming);
        app_ui_event_t e = input(&m, MENU_INPUT_SELECT);
        assert(e.action == APP_BLE && !m.open);

        menu_model_set_mode(&m, tasks[i]);
        choose_index(&m, MENU_ITEM_COUNT - 1);
        input(&m, MENU_INPUT_SELECT);
        assert(m.confirming && !m.confirm_yes);
        menu_model_set_mode(&m, APP_CLOCK);
        assert(m.open && m.confirming && !m.confirm_yes);
        assert(m.selected == MENU_ITEM_COUNT - 1);
        input(&m, MENU_INPUT_DOWN);
        menu_model_set_mode(&m, APP_CLOCK);
        assert(m.open && m.confirming && m.confirm_yes);
        e = input(&m, MENU_INPUT_SELECT);
        assert(e.action == APP_INITIALIZE && !m.open);
        menu_model_open(&m);
        assert(m.open && m.selected == 0 && !m.confirming);
    }
}

static void test_debounce_and_no_repeat(void)
{
    menu_keys_t k;
    const unsigned up = MENU_PRESSED(MENU_KEY_UP);
    const unsigned down = MENU_PRESSED(MENU_KEY_DOWN);
    menu_keys_init(&k, 0, 0);
    assert(menu_keys_sample(&k, up, 20) == 0);
    assert(menu_keys_sample(&k, 0, 40) == 0); /* Contact bounce. */
    assert(menu_keys_sample(&k, up, 60) == 0);
    assert(menu_keys_sample(&k, up, 80) == 0);
    assert(menu_keys_sample(&k, up, 100) == MENU_INPUT_UP);
    for (uint32_t t = 120; t < 2000; t += 20) assert(menu_keys_sample(&k, up, t) == 0);
    assert(menu_keys_sample(&k, 0, 2000) == 0);
    assert(menu_keys_sample(&k, 0, 2040) == 0);
    assert(menu_keys_sample(&k, down, 2060) == 0);
    assert(menu_keys_sample(&k, down, 2100) == MENU_INPUT_DOWN);
    assert(menu_keys_sample(&k, 0, 2120) == 0);
    assert(menu_keys_sample(&k, 0, 2160) == 0);
}

static void test_center_short_and_long(void)
{
    menu_keys_t k;
    unsigned center = MENU_PRESSED(MENU_KEY_CENTER);
    menu_keys_init(&k, 0, 0);
    assert(menu_keys_sample(&k, center, 20) == 0);
    assert(menu_keys_sample(&k, center, 60) == 0);
    assert(menu_keys_sample(&k, 0, 100) == 0);
    assert(menu_keys_sample(&k, center, 120) == 0); /* Release bounce. */
    assert(menu_keys_sample(&k, 0, 140) == 0);
    assert(menu_keys_sample(&k, 0, 180) == MENU_INPUT_SELECT);
    assert(menu_keys_sample(&k, 0, 200) == 0);
    assert(menu_keys_sample(&k, center, 220) == 0);
    assert(menu_keys_sample(&k, center, 260) == 0);
    for (uint32_t t = 280; t < 1260; t += 20) assert(menu_keys_sample(&k, center, t) == 0);
    assert(menu_keys_sample(&k, center, 1260) == MENU_INPUT_LONG);
    for (uint32_t t = 1280; t < 2000; t += 20) assert(menu_keys_sample(&k, center, t) == 0);
    assert(menu_keys_sample(&k, 0, 2000) == 0);
    assert(menu_keys_sample(&k, 0, 2040) == 0); /* No short release after long. */
    assert(menu_keys_sample(&k, center, 2100) == 0);
    assert(menu_keys_sample(&k, center, 2140) == 0);
    assert(menu_keys_sample(&k, 0, 2200) == 0);
    assert(menu_keys_sample(&k, 0, 2240) == MENU_INPUT_SELECT);
}

static void test_key_model_integration(void)
{
    menu_keys_t keys;
    menu_model_t m;
    unsigned center = MENU_PRESSED(MENU_KEY_CENTER);
    menu_keys_init(&keys, 0, 0);
    menu_model_init(&m, APP_LAN);
    assert(menu_keys_sample(&keys, center, 20) == 0);
    assert(menu_keys_sample(&keys, center, 60) == 0);
    unsigned raw = menu_keys_sample(&keys, center, 1060);
    assert(raw == MENU_INPUT_LONG);
    app_ui_event_t e = input(&m, (menu_input_t)raw);
    assert(e.menu_open && e.action == APP_NONE && !e.select);
    assert(m.selected == 1);
    assert(menu_keys_sample(&keys, 0, 1080) == 0);
    assert(menu_keys_sample(&keys, 0, 1120) == 0);
    assert(m.open); /* Releasing long press must not choose current LAN. */
    e = input(&m, MENU_INPUT_DOWN);
    assert(m.selected == 2 && e.action == APP_NONE && !e.select);
    e = input(&m, MENU_INPUT_SELECT);
    assert(e.action == APP_BLE && !e.menu_open && !e.select);
    menu_model_set_mode(&m, APP_DIAGNOSTICS);
    e = input(&m, MENU_INPUT_SELECT);
    assert(e.select && e.action == APP_NONE && !e.menu_open);
    menu_model_open(&m); /* Parent handles diagnostic-result select. */
    assert(m.open && m.selected == 3);
    e = input(&m, MENU_INPUT_SELECT);
    assert(e.action == APP_DIAGNOSTICS); /* Run diagnostics again. */
    menu_model_set_mode(&m, APP_INITIALIZE);
    e = input(&m, MENU_INPUT_SELECT);
    assert(e.select && e.action == APP_NONE && !e.menu_open); /* Resume BLE. */
}

static void test_boot_hold_and_wrap(void)
{
    for (unsigned key = 0; key < MENU_KEY_COUNT; ++key) {
        menu_keys_t k;
        unsigned pressed = MENU_PRESSED(key);
        menu_keys_init(&k, pressed, 0);
        for (uint32_t t = 0; t <= 3000; t += 20) assert(menu_keys_sample(&k, pressed, t) == 0);
        assert(menu_keys_sample(&k, 0, 3020) == 0);
        assert(menu_keys_sample(&k, 0, 3060) == 0);
        assert(menu_keys_sample(&k, pressed, 3100) == 0);
        unsigned e = menu_keys_sample(&k, pressed, 3140);
        assert(e == (key == MENU_KEY_UP ? MENU_INPUT_UP : key == MENU_KEY_DOWN ? MENU_INPUT_DOWN : 0u));
        assert(menu_keys_sample(&k, 0, 3200) == 0);
        assert(menu_keys_sample(&k, 0, 3240) == (key == MENU_KEY_CENTER ? MENU_INPUT_SELECT : 0u));
    }
    menu_keys_t k;
    uint32_t t = UINT32_MAX - 79u;
    unsigned center = MENU_PRESSED(MENU_KEY_CENTER);
    menu_keys_init(&k, 0, t);
    assert(menu_keys_sample(&k, center, t + 20u) == 0);
    assert(menu_keys_sample(&k, center, t + 60u) == 0);
    assert(menu_keys_sample(&k, center, t + 1040u) == 0);
    assert(menu_keys_sample(&k, center, t + 1060u) == MENU_INPUT_LONG);
    assert(menu_keys_sample(&k, 0, t + 1080u) == 0);
    assert(menu_keys_sample(&k, 0, t + 1120u) == 0);
}

int main(void)
{
    test_navigation();
    test_single_select_and_same_mode();
    test_initialization_confirmation();
    test_background_completion_preserves_menu();
    test_debounce_and_no_repeat();
    test_center_short_and_long();
    test_boot_hold_and_wrap();
    test_key_model_integration();
    puts("menu_test: OK");
    return 0;
}
