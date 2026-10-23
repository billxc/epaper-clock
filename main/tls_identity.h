#pragma once
#include "esp_err.h"
#include <stdint.h>

// Dedicated clock_tls/identity NVS blob. Never reset by normal initialization.
// Call only from management owner after Wi-Fi starts (hardware RNG entropy).
// Not encrypted at rest unless platform NVS encryption is separately enabled.
typedef struct {
    uint32_t version;
    char certificate[2048];
    char private_key[512];
} tls_identity_t;

// Loads the original identity even if ip changed. First-generation SAN has ip
// and epaper-<station MAC>.local; this module does not advertise mDNS.
// A changed DHCP IP may cause a browser name warning, not key regeneration.
// Fails closed on damaged NVS or failed commit; *out remains NULL.
esp_err_t tls_identity_load(const char *ip, tls_identity_t **out);
void tls_identity_free(tls_identity_t *identity);
