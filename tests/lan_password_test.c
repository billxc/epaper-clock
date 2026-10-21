// Host-only NVS fault injection. No real credentials or target hardware.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../main/lan_password.c"

static char stored[32];
static bool exists;
static int writes, commits, random_calls, rw_opens;
static esp_err_t open_error, read_error, set_error, commit_error;
static void reset(void)
{
    memset(stored, 0, sizeof(stored));
    exists = false;
    writes = commits = random_calls = rw_opens = 0;
    open_error = read_error = set_error = commit_error = ESP_OK;
    initialization_error = ESP_OK; // Emulate a fresh boot for each scenario.
}
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{
    assert(!strcmp(name, "clock_lan"));
    if (open_error) return open_error;
    if (mode == NVS_READWRITE) ++rw_opens;
    *handle = mode;
    return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *size)
{
    assert(h == NVS_READONLY && !strcmp(key, "password"));
    if (read_error) return read_error;
    if (!exists) return ESP_ERR_NVS_NOT_FOUND;
    size_t length = strlen(stored) + 1;
    if (length > *size) return ESP_FAIL;
    memcpy(out, stored, length);
    *size = length;
    return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value)
{
    assert(h == NVS_READWRITE && !strcmp(key, "password"));
    ++writes;
    if (set_error) return set_error;
    // Deliberately expose a set value even if commit subsequently fails.
    strcpy(stored, value);
    exists = true;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h)
{
    assert(h == NVS_READWRITE);
    ++commits;
    return commit_error;
}
void nvs_close(nvs_handle_t h) { (void)h; }
void esp_fill_random(void *out, size_t size)
{
    unsigned char *bytes = out;
    ++random_calls;
    for (size_t i = 0; i < size; ++i) bytes[i] = (unsigned char)(i * 17);
}
static void failed(char out[17], const char *resume)
{
    memset(out, '!', 17);
    assert(lan_password_get(out, resume) != ESP_OK);
    for (int i = 0; i < 17; ++i) assert(out[i] == 0);
}
int main(void)
{
    char out[17], first[17];
    reset();
    assert(lan_password_get(out, NULL) == ESP_OK && strlen(out) == 16);
    memcpy(first, out, 17);
    assert(writes == 1 && commits == 1 && random_calls == 1);
    // Re-entry, reconnect, minute boot, normal/force initialization do not
    // own a reset API. Every subsequent lookup is read-only, even after boot.
    for (int i = 0; i < 20; ++i) {
        initialization_error = ESP_OK;
        assert(lan_password_get(out, NULL) == ESP_OK && !strcmp(out, first));
    }
    assert(writes == 1 && commits == 1 && random_calls == 1 && rw_opens == 1);
    assert(lan_password_get(out, first) == ESP_OK);
    failed(out, "0123456789abcdef"); // Resume must not override fixed NVS.
    assert(writes == 1 && commits == 1);

    reset();
    assert(lan_password_get(out, "0123456789abcdef") == ESP_OK);
    assert(!strcmp(out, "0123456789abcdef") && !random_calls && commits == 1);
    assert(lan_password_get(out, "0123456789abcdef") == ESP_OK && commits == 1);

    reset();
    failed(out, "short");
    assert(!writes && !random_calls);
    reset(); exists = true; strcpy(stored, "corrupt");
    failed(out, NULL);
    assert(!writes && !random_calls);
    reset(); read_error = ESP_FAIL;
    failed(out, NULL);
    assert(!writes && !random_calls);
    reset(); open_error = ESP_FAIL;
    failed(out, NULL);
    assert(!writes && !random_calls);
    reset(); set_error = ESP_FAIL;
    failed(out, NULL);
    assert(!commits);
    reset(); commit_error = ESP_FAIL;
    failed(out, NULL);
    commit_error = ESP_OK;
    failed(out, NULL); // Cannot bypass failed commit through NVS's RAM view.
    assert(writes == 1 && commits == 1 && random_calls == 1);
    puts("LAN password persistence/fault tests passed");
    return 0;
}
