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
    APP_POWER_OFF, // One-shot supervisor task, not a business mode/menu item.
} app_action_t;

typedef struct {
    app_action_t action;
    bool activity;
    bool select;
    bool menu_open;
} app_ui_event_t;

// Called on the main task, after epaper_port_init().
void app_ui_init(void);
// Call immediately after init ONLY on GPIO5 EXT1 wake, before any normal page.
// Confirm a continuous ~1s hold without consuming the scanner's boot-key ignore.
// Returns a request; caller must honor OTA/forced-init/recovery priority first.
bool app_ui_confirm_wake_menu(void);
void app_ui_set_mode(app_action_t mode);
app_ui_event_t app_ui_poll(void);
bool app_ui_menu_open(void);
bool app_ui_owner(void);
void app_ui_open_menu(void);
// OTA owns the device temporarily; discard rather than replay queued clicks.
void app_ui_discard_input(void);
// Called after the LAN owner applies a mode and replaces the saved background.
void app_ui_refresh_menu(void);
bool app_ui_display_ok(void);
// Force visible prompt even over an open menu, then ordered spatial erasure
// and full white. Existing legal mono/full-gray drivers only, no TF save.
// Returns after final transfer/BUSY and display sleep; false on failure.
bool app_ui_poweroff(void);
// Immediately after present_clock: false when only the hidden background was
// updated beneath a menu. Such a frame must not be persisted as panel state.
bool app_ui_clock_visible(void);
// Frames are packed 2bpp (0 black, 3 white), in the active render mode.
// Preserve a background frame while the menu overlays it.
void app_ui_present(const uint8_t *image, bool full);
// Restore old controller frame and update within ONE powered refresh session.
void app_ui_present_clock(const uint8_t *previous, const uint8_t *image, bool full);
void app_ui_message(const char *title, const char *const *lines, size_t count);
