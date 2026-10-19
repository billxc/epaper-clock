#pragma once
/* Pure C UI policy and sampled-button state; no ESP-IDF dependencies. */
#include "app_ui.h"

#define MENU_ITEM_COUNT 6
#define MENU_DEBOUNCE_MS 40u
#define MENU_LONG_PRESS_MS 1000u

typedef enum {
    MENU_KEY_UP = 0,
    MENU_KEY_CENTER,
    MENU_KEY_DOWN,
    MENU_KEY_COUNT
} menu_key_t;
#define MENU_PRESSED(key) (1u << (key))

typedef enum {
    MENU_INPUT_UP = 1u << 0,
    MENU_INPUT_DOWN = 1u << 1,
    MENU_INPUT_SELECT = 1u << 2,
    MENU_INPUT_LONG = 1u << 3,
} menu_input_t;

typedef struct {
    bool stable, candidate, ignore, long_sent;
    uint32_t changed_at, pressed_at;
} menu_button_t;
typedef struct { menu_button_t buttons[MENU_KEY_COUNT]; } menu_keys_t;

/* pressed is a bitmask of logical pressed keys (electrical active-low is
 * translated by the caller). Keys held at init are ignored until released.
 * Unsigned milliseconds may wrap. Sample regularly, e.g. every 20 ms. */
void menu_keys_init(menu_keys_t *keys, unsigned pressed, uint32_t now_ms);
unsigned menu_keys_sample(menu_keys_t *keys, unsigned pressed, uint32_t now_ms);

typedef struct {
    app_action_t mode;
    unsigned selected;
    bool open, confirming, confirm_yes;
} menu_model_t;

void menu_model_init(menu_model_t *model, app_action_t mode);
/* Updates background mode; preserves an open menu's cursor/confirmation. */
void menu_model_set_mode(menu_model_t *model, app_action_t mode);
void menu_model_open(menu_model_t *model);
/* One input flag per call. select is exposed only outside the menu. No idle
 * auto-close: selections/initialization confirmation must remain deliberate. */
app_ui_event_t menu_model_input(menu_model_t *model, menu_input_t input);
app_action_t menu_model_item(unsigned index);
const char *menu_model_label(unsigned index);
