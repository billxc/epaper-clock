#pragma once
#include <stdbool.h>
// Blocks until exit/timeout; leaves Wi-Fi stopped and handlers finished.
// Returns true only when Wi-Fi configuration changed and needs applying.
bool admin_run(void);
