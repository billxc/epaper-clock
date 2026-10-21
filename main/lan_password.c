#include "lan_password.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "esp_random.h"
#include "nvs.h"

// A failed initialization cannot be bypassed by a later read of NVS's RAM
// view in this boot (set may have succeeded before commit failed).
static esp_err_t initialization_error;

static bool valid(const char *password)
{
    // Existing OTA v1 credentials are 16 hex characters. New credentials use
    // 16 base64url characters (96 random bits), fitting that stable ABI.
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    return password && strnlen(password, 17) == 16 &&
        strspn(password, alphabet) == 16;
}

esp_err_t lan_password_get(char out[17], const char *resume_password)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, 17);
    if (initialization_error != ESP_OK) return initialization_error;
    if (resume_password && !valid(resume_password)) return ESP_ERR_INVALID_STATE;

    char saved[17] = {0};
    nvs_handle_t handle;
    esp_err_t err = nvs_open("clock_lan", NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t size = sizeof(saved);
        err = nvs_get_str(handle, "password", saved, &size);
        nvs_close(handle);
        if (err == ESP_OK) {
            if (size != sizeof(saved) || !valid(saved) ||
                (resume_password && strcmp(saved, resume_password)))
                err = ESP_ERR_INVALID_STATE;
            else memcpy(out, saved, sizeof(saved));
            memset(saved, 0, sizeof(saved));
            return err;
        }
    }
    // Never replace a damaged/inaccessible credential with a random one.
    if (err != ESP_ERR_NVS_NOT_FOUND) return err;

    if (resume_password) memcpy(saved, resume_password, sizeof(saved));
    else {
        static const char alphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        uint8_t random[16];
        esp_fill_random(random, sizeof(random));
        for (unsigned i = 0; i < sizeof(random); ++i)
            saved[i] = alphabet[random[i] & 63]; // 256 is divisible by 64.
        memset(random, 0, sizeof(random));
    }
    err = nvs_open("clock_lan", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "password", saved);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    // No HTTP listener or displayed usable credential before durable commit.
    if (err == ESP_OK) memcpy(out, saved, sizeof(saved));
    else initialization_error = err;
    memset(saved, 0, sizeof(saved));
    return err;
}
