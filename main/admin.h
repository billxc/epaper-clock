#pragma once
#include "app_ui.h"
// Main/UI-owning task only. LAN failures remain in management with a working
// physical menu. Idle 300s (not status polling) returns APP_CLOCK; selecting a
// different menu action returns it after HTTP task join and Wi-Fi cleanup.
// Upload/verification/reboot temporarily disable idle and mode changes.
app_action_t admin_run(void);
// Main/UI task only: app_poll should call this whenever it detects physical
// activity, including activity consumed inside the unified network poll hook.
// Safe/no-op outside admin_run. HTTP operations record their activity internally.
void admin_note_activity(void);
// Main-task poll hook: permits timely cancellation even during STA/NTP waits.
bool admin_idle_expired(void);
