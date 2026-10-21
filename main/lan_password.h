#pragma once
#include "esp_err.h"

// Serialized by the management/supervisor task. RF must be active when a new
// random credential may be generated; adopting an explicit resume needs no RNG.
// Reads an existing credential without writing Flash. Only an absent key may
// be initialized; malformed data/read/commit errors fail closed. A legacy OTA
// resume credential is adopted only when no fixed credential exists; a mismatch
// is an error, never a temporary override. Never log either argument.
esp_err_t lan_password_get(char out[17], const char *resume_password);
