#pragma once
#include "app_ui.h"

// Pumps physical controls on the UI-owning task. A chosen action stays pending
// until the main supervisor completes the current service's cleanup.
app_action_t app_poll(bool *activity, bool *selected);
