#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "render_mode.h"

// Startup/UI owner, after NVS initialization and before starting other tasks.
// Missing settings default to mono; never erases or migrates other namespaces.
void render_settings_init(void);
// HTTP-safe: persist only this setting, then publish the request. Never draws
// or changes the active mode. On failure the previous request is retained.
esp_err_t render_settings_save(render_mode_t mode);
render_mode_t render_settings_requested(void);
// UI owner only, between frames and under the LAN/OTA drawing gate.
bool render_settings_apply(void);
// UI owner only, after a gray refresh timeout. Always forces requested/active
// mono even if persistence fails, preventing another gray attempt each loop.
esp_err_t render_settings_fallback(void);
// Held-up forced initialization is also the display recovery entrance.
esp_err_t render_settings_restore_default(void);
// Stable string literal (empty when healthy); safe to read from HTTP.
const char *render_settings_warning(void);
