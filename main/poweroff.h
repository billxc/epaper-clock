#pragma once
#include <stdbool.h>

// All APIs belong to the supervisor/UI owner, never HTTP or key scanner.
bool poweroff_init(void); // Before display/OTA; failure must stop normal boot.
void poweroff_sample(void); // No drawing; also sampled while OTA discards keys.
bool poweroff_requested(bool ota_protected); // 4s + release; OTA postpones execution.
bool poweroff_tracking(void); // Keep ordinary minute sleep from stealing a hold.
// After management tasks have joined and OTA boot/recovery checks succeeded.
// Returns false on failure; caller MUST stay awake in recovery, not clock sleep.
bool poweroff_execute(void);
