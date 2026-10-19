#include "menu_model.h"
#include <string.h>

static const app_action_t items[MENU_ITEM_COUNT] = {
    APP_CLOCK, APP_LAN, APP_BLE, APP_DIAGNOSTICS, APP_SYNC, APP_INITIALIZE,
};
static const char *const labels[MENU_ITEM_COUNT] = {
    "时钟模式", "LAN 管理模式", "蓝牙管理模式", "本机自动调试",
    "立刻自动对时", "初始化／配网",
};

app_action_t menu_model_item(unsigned index)
{
    return index < MENU_ITEM_COUNT ? items[index] : APP_NONE;
}

const char *menu_model_label(unsigned index)
{
    return index < MENU_ITEM_COUNT ? labels[index] : "";
}

static unsigned mode_index(app_action_t mode)
{
    for (unsigned i = 0; i < MENU_ITEM_COUNT; ++i)
        if (items[i] == mode) return i;
    return 0;
}

void menu_model_init(menu_model_t *model, app_action_t mode)
{
    memset(model, 0, sizeof(*model));
    menu_model_set_mode(model, mode);
}

void menu_model_set_mode(menu_model_t *model, app_action_t mode)
{
    model->mode = mode;
    /* A background task may finish while the user is navigating. Preserve
     * the live menu/confirmation; the next explicit open follows new mode. */
    if (!model->open) model->selected = mode_index(mode);
}

void menu_model_open(menu_model_t *model)
{
    model->open = true;
    model->confirming = false;
    model->confirm_yes = false;
    model->selected = mode_index(model->mode);
}

app_ui_event_t menu_model_input(menu_model_t *model, menu_input_t input)
{
    app_ui_event_t event = {0};
    event.activity = input == MENU_INPUT_UP || input == MENU_INPUT_DOWN ||
                     input == MENU_INPUT_SELECT || input == MENU_INPUT_LONG;
    if (input == MENU_INPUT_LONG) {
        if (!model->open) menu_model_open(model);
    } else if (!model->open) {
        event.select = input == MENU_INPUT_SELECT;
    } else if (input == MENU_INPUT_UP || input == MENU_INPUT_DOWN) {
        if (model->confirming) model->confirm_yes = !model->confirm_yes;
        else if (input == MENU_INPUT_UP)
            model->selected = (model->selected + MENU_ITEM_COUNT - 1) % MENU_ITEM_COUNT;
        else model->selected = (model->selected + 1) % MENU_ITEM_COUNT;
    } else if (input == MENU_INPUT_SELECT) {
        app_action_t chosen = items[model->selected];
        if (model->confirming) {
            model->confirming = false;
            if (model->confirm_yes) {
                model->open = false;
                if (model->mode != APP_INITIALIZE) event.action = APP_INITIALIZE;
            }
            model->confirm_yes = false;
        } else if (chosen == APP_INITIALIZE) {
            model->confirming = true;
            model->confirm_yes = false;
        } else {
            model->open = false;
            /* One-shot tasks are always repeatable; only mode entries can be
             * selected without restarting the current service. */
            if (chosen != model->mode || chosen == APP_DIAGNOSTICS || chosen == APP_SYNC)
                event.action = chosen;
        }
    }
    event.menu_open = model->open;
    return event;
}

void menu_keys_init(menu_keys_t *keys, unsigned pressed, uint32_t now_ms)
{
    memset(keys, 0, sizeof(*keys));
    for (unsigned i = 0; i < MENU_KEY_COUNT; ++i) {
        menu_button_t *b = &keys->buttons[i];
        b->stable = b->candidate = b->ignore = (pressed & MENU_PRESSED(i)) != 0;
        b->changed_at = b->pressed_at = now_ms;
    }
}

unsigned menu_keys_sample(menu_keys_t *keys, unsigned pressed, uint32_t now_ms)
{
    unsigned events = 0;
    for (unsigned i = 0; i < MENU_KEY_COUNT; ++i) {
        menu_button_t *b = &keys->buttons[i];
        bool down = (pressed & MENU_PRESSED(i)) != 0;
        if (down != b->candidate) {
            b->candidate = down;
            b->changed_at = now_ms;
        }
        if (b->stable != b->candidate &&
            (uint32_t)(now_ms - b->changed_at) >= MENU_DEBOUNCE_MS) {
            b->stable = b->candidate;
            if (b->stable) {
                b->pressed_at = now_ms;
                b->long_sent = false;
                if (!b->ignore && i != MENU_KEY_CENTER)
                    events |= i == MENU_KEY_UP ? MENU_INPUT_UP : MENU_INPUT_DOWN;
            } else {
                if (!b->ignore && i == MENU_KEY_CENTER && !b->long_sent)
                    events |= MENU_INPUT_SELECT;
                b->ignore = false;
                b->long_sent = false;
            }
        }
        /* Do not start a long press while a release is being debounced. */
        if (i == MENU_KEY_CENTER && b->stable && b->candidate && !b->ignore &&
            !b->long_sent && (uint32_t)(now_ms - b->pressed_at) >= MENU_LONG_PRESS_MS) {
            b->long_sent = true;
            events |= MENU_INPUT_LONG;
        }
    }
    return events;
}
