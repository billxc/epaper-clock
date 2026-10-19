#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    APP_NONE = 0,
    APP_CLOCK,
    APP_LAN,
    APP_BLE,
    APP_DIAGNOSTICS,
    APP_SYNC,
    APP_INITIALIZE,
} app_action_t;

typedef struct {
    app_action_t action;
    bool activity;
    bool select;
    bool menu_open;
} app_ui_event_t;

// Called on the main task, after epaper_port_init().
void app_ui_init(void);
void app_ui_set_mode(app_action_t mode);
app_ui_event_t app_ui_poll(void);
bool app_ui_menu_open(void);
bool app_ui_owner(void);
void app_ui_open_menu(void);
// OTA owns the device temporarily; discard rather than replay queued clicks.
void app_ui_discard_input(void);
// Preserve a background frame while the menu overlays it.
void app_ui_present(const uint8_t *image, bool full);
// Restore old controller frame and update within ONE powered refresh session.
void app_ui_present_clock(const uint8_t *previous, const uint8_t *image, bool full);
void app_ui_message(const char *title, const char *const *lines, size_t count);
