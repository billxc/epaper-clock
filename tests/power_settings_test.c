// Host-only persistence/fault/concurrent-snapshot tests. No hardware access.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../main/power_settings.c"

static pthread_mutex_t host_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool allocation_fail, exists, pending;
static uint8_t stored[32], staged[32];
static size_t stored_size, staged_size;
static unsigned writes, commits, rw_opens;
static esp_err_t open_error, read_error, set_error, commit_error;
static atomic_bool stop_reader;

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return allocation_fail ? NULL : &host_mutex; }
int xSemaphoreTake(SemaphoreHandle_t m, int wait) { (void)wait; return pthread_mutex_lock(m) == 0; }
int xSemaphoreGive(SemaphoreHandle_t m) { return pthread_mutex_unlock(m) == 0; }
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *h)
{
    assert(!strcmp(name, "clock_power")); // Never touches Wi-Fi/RTC/LAN/UI.
    if (open_error) return open_error;
    if (mode == NVS_READWRITE) ++rw_opens;
    *h = mode;
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *size)
{
    assert(h == NVS_READONLY && !strcmp(key, "settings"));
    if (read_error) return read_error;
    if (!exists) return ESP_ERR_NVS_NOT_FOUND;
    if (stored_size > *size) return ESP_FAIL;
    memcpy(out, stored, stored_size);
    *size = stored_size;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *data, size_t size)
{
    assert(h == NVS_READWRITE && !strcmp(key, "settings") && size == 8);
    ++writes;
    if (set_error) return set_error;
    memcpy(staged, data, size);
    staged_size = size;
    pending = true;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h)
{
    assert(h == NVS_READWRITE && pending);
    ++commits;
    if (commit_error) return commit_error;
    // Check the old snapshot is still published at the commit boundary.
    assert(pack(power_settings_get()) == atomic_load(&snapshot));
    memcpy(stored, staged, staged_size);
    stored_size = staged_size;
    exists = true;
    pending = false;
    return ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; pending = false; }
static void expect(power_sleep_mode_t mode, unsigned ms)
{
    power_settings_t s = power_settings_get();
    assert(s.sleep_mode == mode && s.pwr_on_ms == ms);
}
static void reset(void)
{
    exists = pending = allocation_fail = false;
    writes = commits = rw_opens = 0;
    open_error = read_error = set_error = commit_error = ESP_OK;
    stored_size = 0;
    persisted_known = false;
    mutex = NULL;
    atomic_store(&snapshot, 1000u << 1);
    atomic_store(&warning, "");
}
static void *reader(void *unused)
{
    (void)unused;
    while (!atomic_load(&stop_reader)) {
        power_settings_t s = power_settings_get();
        assert((s.sleep_mode == POWER_SLEEP_DEEP && s.pwr_on_ms == 128) ||
               (s.sleep_mode == POWER_SLEEP_PMU && s.pwr_on_ms == 2000));
        assert(power_settings_warning() != NULL);
    }
    return NULL;
}
int main(void)
{
    reset();
    assert(power_settings_save((power_settings_t){POWER_SLEEP_PMU, 1000}) == ESP_ERR_INVALID_STATE);
    allocation_fail = true;
    power_settings_init();
    expect(POWER_SLEEP_PMU, 1000);
    assert(*power_settings_warning());
    allocation_fail = false;
    power_settings_init();
    expect(POWER_SLEEP_PMU, 1000);
    assert(!*power_settings_warning() && !writes && !commits);
    for (int i = 0; i < 5; ++i) {
        assert(power_settings_save((power_settings_t){POWER_SLEEP_PMU, 1000}) == ESP_OK);
        power_settings_init();
    }
    assert(!writes && !commits && !rw_opens);
    assert(power_settings_save((power_settings_t){POWER_SLEEP_DEEP, 512}) == ESP_OK);
    assert(writes == 1 && commits == 1);
    expect(POWER_SLEEP_DEEP, 512);
    assert(stored[0] == 1 && stored[1] == POWER_SLEEP_DEEP && stored[2] == 0 && stored[3] == 2);
    for (int i = 0; i < 10; ++i) {
        // Re-entry/ordinary/forced init must read the same independent record.
        power_settings_init();
        expect(POWER_SLEEP_DEEP, 512);
        assert(power_settings_save(power_settings_get()) == ESP_OK);
    }
    assert(writes == 1 && commits == 1);

    power_settings_t bad[] = {{-1, 1000}, {2, 1000}, {POWER_SLEEP_PMU, 0},
        {POWER_SLEEP_DEEP, 127}, {POWER_SLEEP_PMU, 129}, {POWER_SLEEP_PMU, 999},
        {POWER_SLEEP_PMU, 2001}, {POWER_SLEEP_PMU, ~0u}};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        assert(power_settings_save(bad[i]) == ESP_ERR_INVALID_ARG);
    assert(writes == 1 && commits == 1);
    expect(POWER_SLEEP_DEEP, 512);

    esp_err_t *errors[] = {&open_error, &set_error, &commit_error};
    for (unsigned i = 0; i < 3; ++i) {
        *errors[i] = ESP_FAIL;
        assert(power_settings_save((power_settings_t){POWER_SLEEP_PMU, 2000}) == ESP_FAIL);
        expect(POWER_SLEEP_DEEP, 512);
        assert(*power_settings_warning());
        *errors[i] = ESP_OK;
        // Unknown persistence after failure must not suppress retries.
        unsigned old_writes = writes;
        assert(power_settings_save((power_settings_t){POWER_SLEEP_DEEP, 512}) == ESP_OK);
        assert(writes == old_writes + 1 && !*power_settings_warning());
    }

    // Bad version/mode/threshold/reserved bytes/record lengths fail safely.
    const uint8_t good[8] = {1, 1, 0, 2, 0, 0, 0, 0};
    for (int field = 0; field < 8; ++field) {
        memcpy(stored, good, 8); stored_size = 8; exists = true;
        stored[field] = field == 1 ? 2 : 3;
        power_settings_init();
        expect(POWER_SLEEP_PMU, 1000);
        assert(*power_settings_warning());
    }
    memcpy(stored, good, 8);
    stored_size = 7;
    power_settings_init(); expect(POWER_SLEEP_PMU, 1000);
    assert(*power_settings_warning());
    stored_size = 9;
    power_settings_init(); expect(POWER_SLEEP_PMU, 1000);
    assert(*power_settings_warning());
    read_error = ESP_FAIL;
    power_settings_init(); expect(POWER_SLEEP_PMU, 1000);
    assert(*power_settings_warning());
    read_error = ESP_OK;
    assert(power_settings_save((power_settings_t){POWER_SLEEP_PMU, 1000}) == ESP_OK);
    assert(!*power_settings_warning());
    power_settings_init(); expect(POWER_SLEEP_PMU, 1000);

    assert(power_settings_save((power_settings_t){POWER_SLEEP_PMU, 2000}) == ESP_OK);
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, reader, NULL));
    for (int i = 0; i < 10000; ++i) {
        power_settings_t s = i & 1 ? (power_settings_t){POWER_SLEEP_PMU, 2000} :
                                    (power_settings_t){POWER_SLEEP_DEEP, 128};
        assert(power_settings_save(s) == ESP_OK);
    }
    atomic_store(&stop_reader, true);
    assert(!pthread_join(thread, NULL));
    puts("Power settings defaults/persistence/no-op/fault/validation/atomic snapshot tests passed");
    return 0;
}
