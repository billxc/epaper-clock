#include "ota.h"
#include "lan_password.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "nvs.h"
#include "mbedtls/sha256.h"

// A single committed blob keeps hash/version/credential transactional. Keep
// this v1 layout/magic stable across OTA-capable releases: the rollback image
// must read a transaction written by its successor. This is not a signing
// mechanism: Basic HTTP is suitable ONLY on a trusted LAN.
#define RECORD_MAGIC 0x4f544131u
typedef struct {
    uint32_t magic;
    uint32_t target_address;
    uint8_t hash[32];
    char version[33];
    char password[17];
} resume_record_t;
static resume_record_t record;
static bool resume;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static ota_status_t status = {.state = "idle"};
static bool busy, reboot;
static int64_t reboot_at;
// The following stream fields belong exclusively to the HTTP task.
static esp_ota_handle_t writer;
static bool writer_open, header_checked;
static const esp_partition_t *target;
#define PREFIX_SIZE (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))
static uint8_t prefix[PREFIX_SIZE];
static size_t prefix_used, received, total;
static char password[17];
static char version[33];
static mbedtls_sha256_context upload_hash;
static bool hash_open, hash_expected;
static uint8_t expected_hash[32];

static void state(const char *name, const char *message)
{
    portENTER_CRITICAL(&lock);
    snprintf(status.state, sizeof(status.state), "%s", name);
    snprintf(status.message, sizeof(status.message), "%s", message);
    portEXIT_CRITICAL(&lock);
}
static esp_err_t store_record(const resume_record_t *value)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("clock_ota", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(nvs, "resume", value, sizeof(*value));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}
void ota_management_finished(void)
{
    if (!ota_resume_management() || ota_busy()) return;
    // Force-initialize and leaving recovery must not discard the only copy of
    // a legacy credential before adopting it. No entropy is needed: a resume
    // credential is mandatory here. Conflicts/errors retain the transaction.
    char legacy[17] = {0}, fixed[17] = {0};
    if (!ota_get_password_override(legacy)) return;
    esp_err_t credential = lan_password_get(fixed, legacy);
    memset(legacy, 0, sizeof(legacy));
    memset(fixed, 0, sizeof(fixed));
    if (credential != ESP_OK) return;
    nvs_handle_t nvs;
    if (nvs_open("clock_ota", NVS_READWRITE, &nvs) != ESP_OK) return;
    esp_err_t err = nvs_erase_key(nvs, "resume");
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        portENTER_CRITICAL(&lock);
        resume = false;
        memset(&record, 0, sizeof(record));
        portEXIT_CRITICAL(&lock);
    }
}
bool ota_boot_check(bool healthy)
{
    // No NVS write on routine minute boots. Read-only lookup is intentional.
    nvs_handle_t nvs;
    size_t size = sizeof(record);
    if (nvs_open("clock_ota", NVS_READONLY, &nvs) == ESP_OK) {
        esp_err_t err = nvs_get_blob(nvs, "resume", &record, &size);
        nvs_close(nvs);
        resume = err == ESP_OK && size == sizeof(record) &&
            record.magic == RECORD_MAGIC && record.password[16] == 0 &&
            strlen(record.password) == 16 && record.version[32] == 0;
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t image_state;
    if (esp_ota_get_state_partition(running, &image_state) == ESP_OK &&
        image_state == ESP_OTA_IMG_PENDING_VERIFY) {
        if (!healthy) {
            state("error", "Mandatory hardware health failed; rolling back");
            esp_ota_mark_app_invalid_rollback_and_reboot();
            // No valid fallback or SDK error: never silently bless this image.
            state("error", "Health failed; automatic rollback unavailable");
            return false;
        }
        if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
            state("error", "Unable to confirm boot health; rolling back");
            esp_ota_mark_app_invalid_rollback_and_reboot();
            return false; // Never enter the minute poweroff path unconfirmed.
        }
    }
    if (resume) {
        uint8_t hash[32];
        esp_err_t hash_result = esp_partition_get_sha256(running, hash);
        if (hash_result != ESP_OK) {
            state("error", "Cannot verify running application hash");
            return true; // App is confirmed; report the resume-result error.
        }
        // Address disambiguates rollback even when both slots contain the
        // exact same binary/version (e.g. a deliberate reinstall).
        bool match = running->address == record.target_address &&
            memcmp(hash, record.hash, sizeof(hash)) == 0;
        portENTER_CRITICAL(&lock);
        memcpy(status.expected_version, record.version, sizeof(record.version));
        portEXIT_CRITICAL(&lock);
        state(match ? "success" : "rollback", match ?
              "New firmware booted and passed local health checks" :
              "Previous firmware restored (rollback or interrupted activation)");
    }
    return true;
}
bool ota_resume_management(void)
{
    portENTER_CRITICAL(&lock);
    bool value = resume;
    portEXIT_CRITICAL(&lock);
    return value;
}
bool ota_get_password_override(char out[17])
{
    portENTER_CRITICAL(&lock);
    bool value = resume;
    if (value) memcpy(out, record.password, 17);
    portEXIT_CRITICAL(&lock);
    return value;
}
void ota_get_status(ota_status_t *out)
{
    portENTER_CRITICAL(&lock);
    *out = status;
    portEXIT_CRITICAL(&lock);
}
bool ota_busy(void)
{
    portENTER_CRITICAL(&lock);
    bool value = busy;
    portEXIT_CRITICAL(&lock);
    return value;
}
bool ota_reboot_requested(void)
{
    portENTER_CRITICAL(&lock);
    bool value = reboot && esp_timer_get_time() >= reboot_at;
    portEXIT_CRITICAL(&lock);
    return value;
}
void ota_response_sent(void)
{
    portENTER_CRITICAL(&lock);
    reboot_at = esp_timer_get_time() + 1500000;
    reboot = busy && !strcmp(status.state, "rebooting");
    portEXIT_CRITICAL(&lock);
}
void ota_abort(const char *reason)
{
    if (writer_open) esp_ota_abort(writer);
    writer_open = false;
    if (hash_open) mbedtls_sha256_free(&upload_hash);
    hash_open = hash_expected = false;
    memset(password, 0, sizeof(password));
    state("error", reason);
    portENTER_CRITICAL(&lock);
    busy = false;
    reboot = false;
    portEXIT_CRITICAL(&lock);
}
static esp_err_t fail(const char *reason, esp_err_t err)
{
    ota_abort(reason);
    return err == ESP_OK ? ESP_FAIL : err;
}
esp_err_t ota_begin(size_t length, const char *credential, const char *sha256)
{
    if (ota_busy()) return ESP_ERR_INVALID_STATE;
    target = esp_ota_get_next_update_partition(NULL);
    if (!target || target == esp_ota_get_running_partition() ||
        target->type != ESP_PARTITION_TYPE_APP ||
        (target->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
         target->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_1))
        return fail("No inactive OTA application partition", ESP_ERR_NOT_FOUND);
    if (length < PREFIX_SIZE || length > target->size)
        return fail("Application binary is too small or exceeds target partition", ESP_ERR_INVALID_SIZE);
    if (!credential || strlen(credential) != 16)
        return fail("Management credential unavailable", ESP_ERR_INVALID_STATE);
    // Optional for legacy manual uploads; a supplied checksum must be exact.
    // This is transport integrity, NOT a publisher signature.
    hash_expected = sha256 != NULL;
    if (hash_expected) {
        if (strlen(sha256) != 64)
            return fail("Expected SHA256 must be 64 hexadecimal characters", ESP_ERR_INVALID_ARG);
        for (size_t i = 0; i < 32; ++i) {
            unsigned value = 0;
            for (size_t j = 0; j < 2; ++j) {
                char c = sha256[i * 2 + j];
                unsigned digit;
                if (c >= '0' && c <= '9') digit = (unsigned)(c - '0');
                else if (c >= 'a' && c <= 'f') digit = (unsigned)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') digit = (unsigned)(c - 'A' + 10);
                else return fail("Expected SHA256 is not hexadecimal", ESP_ERR_INVALID_ARG);
                value = value * 16 + digit;
            }
            expected_hash[i] = (uint8_t)value;
        }
        mbedtls_sha256_init(&upload_hash);
        hash_open = true;
        if (mbedtls_sha256_starts(&upload_hash, 0) != 0)
            return fail("Cannot initialize upload SHA256", ESP_FAIL);
    }
    total = length;
    received = prefix_used = 0;
    writer_open = header_checked = false;
    memcpy(password, credential, sizeof(password));
    portENTER_CRITICAL(&lock);
    busy = true;
    reboot = false;
    status.received = 0;
    status.total = length;
    status.expected_version[0] = 0;
    portEXIT_CRITICAL(&lock);
    state("uploading", "Uploading application; do not disconnect power");
    return ESP_OK;
}
static esp_err_t check_header(void)
{
    esp_image_header_t image;
    esp_app_desc_t app;
    memcpy(&image, prefix, sizeof(image));
    memcpy(&app, prefix + sizeof(image) + sizeof(esp_image_segment_header_t), sizeof(app));
    const esp_app_desc_t *current = esp_app_get_description();
    // Chip ID is from the running application header, not a hand-maintained
    // chip table. esp_ota_end additionally verifies chip revisions and image.
    esp_image_header_t running_header;
    esp_err_t err = esp_partition_read(esp_ota_get_running_partition(), 0,
                                       &running_header, sizeof(running_header));
    if (err != ESP_OK) return fail("Cannot read current application header", err);
    if (image.magic != ESP_IMAGE_HEADER_MAGIC || image.chip_id != running_header.chip_id ||
        app.magic_word != ESP_APP_DESC_MAGIC_WORD ||
        !memchr(app.project_name, 0, sizeof(app.project_name)) ||
        !memchr(app.version, 0, sizeof(app.version)) ||
        strcmp(app.project_name, current->project_name))
        return fail("Wrong chip/project or not a standalone application binary", ESP_ERR_INVALID_ARG);
    snprintf(version, sizeof(version), "%s", app.version);
    // Begin only after rejecting obvious non-app, bootloader and merged images.
    err = esp_ota_begin(target, total, &writer);
    if (err != ESP_OK) return fail("Cannot prepare inactive partition", err);
    writer_open = true;
    err = esp_ota_write(writer, prefix, sizeof(prefix));
    if (err != ESP_OK) return fail("Writing application header failed", err);
    header_checked = true;
    portENTER_CRITICAL(&lock);
    snprintf(status.expected_version, sizeof(status.expected_version), "%s", version);
    portEXIT_CRITICAL(&lock);
    return ESP_OK;
}
esp_err_t ota_write(const void *data, size_t length)
{
    if (!ota_busy() || length > total - received) return fail("Invalid upload length", ESP_ERR_INVALID_SIZE);
    if (hash_expected && mbedtls_sha256_update(&upload_hash, data, length) != 0)
        return fail("Upload SHA256 failed", ESP_FAIL);
    const uint8_t *bytes = data;
    size_t left = length;
    if (!header_checked) {
        size_t take = sizeof(prefix) - prefix_used;
        if (take > left) take = left;
        memcpy(prefix + prefix_used, bytes, take);
        prefix_used += take;
        bytes += take;
        left -= take;
        if (prefix_used == sizeof(prefix)) {
            esp_err_t err = check_header();
            if (err != ESP_OK) return err;
        }
    }
    if (left) {
        esp_err_t err = esp_ota_write(writer, bytes, left);
        if (err != ESP_OK) return fail("Flash write failed", err);
    }
    received += length;
    portENTER_CRITICAL(&lock);
    status.received = received;
    portEXIT_CRITICAL(&lock);
    return ESP_OK;
}
esp_err_t ota_finish(void)
{
    if (!writer_open || received != total)
        return fail("Upload interrupted or incomplete", ESP_ERR_INVALID_SIZE);
    // Compare before esp_ota_end, recovery writes, and boot selection. On any
    // mismatch abort the open writer; the running/boot partition stays intact.
    if (hash_expected) {
        uint8_t actual[32];
        if (mbedtls_sha256_finish(&upload_hash, actual) != 0)
            return fail("Cannot finish upload SHA256", ESP_FAIL);
        mbedtls_sha256_free(&upload_hash);
        hash_open = false;
        if (memcmp(actual, expected_hash, sizeof(actual)) != 0)
            return fail("Downloaded application SHA256 mismatch; boot unchanged", ESP_ERR_INVALID_ARG);
        hash_expected = false;
    }
    state("verifying", "Verifying image; do not disconnect power");
    esp_err_t err = esp_ota_end(writer);
    writer_open = false; // end frees its handle even on verification failure.
    if (err != ESP_OK) return fail("Image validation failed (format/chip/revision/checksum)", err);
    resume_record_t next = {.magic = RECORD_MAGIC, .target_address = target->address};
    err = esp_partition_get_sha256(target, next.hash);
    if (err != ESP_OK) return fail("Cannot obtain verified application hash", err);
    memcpy(next.password, password, sizeof(next.password));
    snprintf(next.version, sizeof(next.version), "%s", version);
    // Persist only AFTER end succeeded, BEFORE changing boot partition.
    err = store_record(&next);
    if (err != ESP_OK) return fail("Cannot persist upgrade recovery; boot unchanged", err);
    portENTER_CRITICAL(&lock);
    record = next;
    resume = true;
    portEXIT_CRITICAL(&lock);
    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ota_abort("Cannot activate verified image; boot unchanged");
        ota_management_finished();
        return err;
    }
    memset(password, 0, sizeof(password));
    state("rebooting", "Verified; restarting into new application");
    return ESP_OK;
}
