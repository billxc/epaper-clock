#include "admin.h"
#include "network.h"
#include "rtc.h"
#include "display.h"
#include "app_runtime.h"
#include "ota.h"
#include "render_settings.h"

// UI-owner API supplied by app_ui; redraws only an already-open menu.
extern void app_ui_refresh_menu(void);
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_random.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "cJSON.h"

extern const char html_start[] asm("_binary_admin_html_start");
extern const char html_end[] asm("_binary_admin_html_end");
// Immutable auth; all fields shared with the HTTP task are atomic.
typedef struct {
    char auth[64];
    char password[17];
    atomic_uint activity;
    atomic_bool exiting;
    atomic_bool syncing;
    // Atomic gate: 0=normal, 1=OTA, 2=ending, 3=owner UI, 4=HTTP save.
    atomic_int gate;
    atomic_bool menu_open;
    SemaphoreHandle_t network_mutex;
    uint32_t boot_id; // Non-secret reconnect discriminator, not an auth token.
    app_action_t selected;
} session_t;
static unsigned seconds(void) { return (unsigned)(esp_timer_get_time() / 1000000); }
// Accessed only on the main/UI task, including the parent's network poll hook.
static session_t *active_session;
void admin_note_activity(void)
{
    if (active_session) atomic_store(&active_session->activity, seconds());
}
bool admin_idle_expired(void)
{
    return active_session && !ota_busy() &&
        seconds() - atomic_load(&active_session->activity) >= 300;
}
static session_t *session(httpd_req_t *r) { return httpd_get_global_user_ctx(r->handle); }
// HTTP server must not free admin_run's stack-owned session on shutdown.
static void keep_session(void *context) { (void)context; }

static esp_err_t reply(httpd_req_t *r, const char *status, const char *body)
{
    httpd_resp_set_status(r, status);
    httpd_resp_set_type(r, "application/json; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_set_hdr(r, "X-Content-Type-Options", "nosniff");
    return httpd_resp_sendstr(r, body);
}
static bool authorized(httpd_req_t *r)
{
    char auth[64];
    size_t n = httpd_req_get_hdr_value_len(r, "Authorization");
    bool ok = n > 0 && n < sizeof(auth) &&
        httpd_req_get_hdr_value_str(r, "Authorization", auth, sizeof(auth)) == ESP_OK &&
        strcmp(auth, session(r)->auth) == 0;
    if (!ok) {
        httpd_resp_set_hdr(r, "WWW-Authenticate", "Basic realm=\"Clock management\"");
        httpd_resp_set_hdr(r, "Connection", "close");
        reply(r, "401 Unauthorized", "{\"error\":\"authentication required\"}");
    }
    return ok;
}
static esp_err_t unknown(httpd_req_t *r, httpd_err_code_t error)
{
    if (!authorized(r)) return ESP_FAIL;
    reply(r, error == HTTPD_405_METHOD_NOT_ALLOWED ? "405 Method Not Allowed" : "404 Not Found",
          "{\"error\":\"not found or unsupported method\"}");
    return ESP_FAIL; // Close: an unhandled request may still have an unread body.
}
static cJSON *body_json(httpd_req_t *r)
{
    char type[80], body[513];
    if (httpd_req_get_hdr_value_str(r, "Content-Type", type, sizeof(type)) != ESP_OK ||
        strncasecmp(type, "application/json", 16) || (type[16] && type[16] != ';') ||
        r->content_len == 0 || r->content_len > 512) return NULL;
    size_t used = 0;
    unsigned started = seconds();
    while (used < r->content_len) {
        if (seconds() - started >= 5) return NULL;
        int n = httpd_req_recv(r, body + used, r->content_len - used);
        if (n <= 0) return NULL;
        used += n;
    }
    body[used] = 0;
    // cJSON strings cannot represent embedded NUL; reject rather than truncate.
    if (memchr(body, 0, used)) return NULL;
    for (size_t i = 0; i + 1 < used; ++i) {
        if (body[i] != '\\') continue;
        ++i; // Skip escaped backslashes, so a literal "\\\\u0000" remains valid.
        if (body[i] == 'u' && i + 4 < used && !memcmp(body + i + 1, "0000", 4)) return NULL;
    }
    cJSON *json = cJSON_ParseWithOpts(body, NULL, true);
    if (!cJSON_IsObject(json)) { cJSON_Delete(json); return NULL; }
    return json;
}
static esp_err_t status_json(httpd_req_t *r)
{
    struct tm now = {0};
    bool valid = rtc_read(&now);
    char time[32] = "", ip[16] = "";
    if (valid) strftime(time, sizeof(time), "%Y-%m-%d %H:%M:%S", &now);
    clock_wifi_config_t config;
    xSemaphoreTake(session(r)->network_mutex, portMAX_DELAY);
    network_get_config(&config);
    network_station_ip(ip, sizeof(ip));
    bool connected = network_connected();
    int64_t last_sync = network_last_sync();
    xSemaphoreGive(session(r)->network_mutex);
    cJSON *json = cJSON_CreateObject();
    if (!json) return reply(r, "500 Internal Server Error", "{\"error\":\"out of memory\"}");
    cJSON_AddStringToObject(json, "rtc_time", time);
    cJSON_AddBoolToObject(json, "rtc_valid", valid);
    cJSON_AddBoolToObject(json, "sta_connected", connected);
    cJSON_AddStringToObject(json, "sta_ip", ip);
    cJSON_AddStringToObject(json, "ssid", config.ssid);
    cJSON_AddNumberToObject(json, "last_sync", (double)last_sync);
    cJSON_AddStringToObject(json, "appversion", esp_app_get_description()->version);
    cJSON_AddNumberToObject(json, "boot_id", session(r)->boot_id);
    cJSON_AddStringToObject(json, "render_mode", render_mode_name(render_settings_requested()));
    cJSON_AddStringToObject(json, "active_render_mode", render_mode_name(render_mode_get()));
    cJSON_AddStringToObject(json, "render_warning", render_settings_warning());
    ota_status_t ota;
    ota_get_status(&ota);
    cJSON *upgrade = cJSON_AddObjectToObject(json, "ota");
    if (upgrade) {
        cJSON_AddStringToObject(upgrade, "state", ota.state);
        cJSON_AddStringToObject(upgrade, "message", ota.message);
        cJSON_AddStringToObject(upgrade, "expected_version", ota.expected_version);
        cJSON_AddNumberToObject(upgrade, "received", (double)ota.received);
        cJSON_AddNumberToObject(upgrade, "total", (double)ota.total);
        cJSON_AddBoolToObject(upgrade, "busy", ota_busy());
    }
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    esp_err_t result = reply(r, text ? "200 OK" : "500 Internal Server Error",
                            text ? text : "{\"error\":\"out of memory\"}");
    cJSON_free(text);
    return result;
}
static esp_err_t upload(httpd_req_t *r)
{
    session_t *s = session(r);
    atomic_store(&s->activity, seconds());
    int expected = 0;
    if (!atomic_compare_exchange_strong(&s->gate, &expected, 1)) {
        httpd_resp_set_hdr(r, "Connection", "close");
        reply(r, "409 Conflict", "{\"error\":\"upgrade busy, screen menu interaction, or session ending; retry when ready\"}");
        return ESP_FAIL;
    }
    // app_ui_present preserves a live menu overlay. Reject instead of hiding
    // the mandatory power warning or reaching into UI internals from HTTP.
    if (atomic_load(&s->menu_open)) {
        atomic_store(&s->gate, 0);
        httpd_resp_set_hdr(r, "Connection", "close");
        reply(r, "409 Conflict", "{\"error\":\"select LAN on the physical menu to close it before uploading\"}");
        return ESP_FAIL;
    }
    char type[64];
    esp_err_t err;
    if (httpd_req_get_hdr_value_str(r, "Content-Type", type, sizeof(type)) != ESP_OK ||
        strcmp(type, "application/octet-stream")) {
        ota_abort("Expected application/octet-stream standalone app bin");
        err = ESP_ERR_INVALID_ARG;
    } else {
        err = ota_begin(r->content_len, s->password);
    }
    uint8_t buffer[4096]; // Never buffer the full image in RAM.
    size_t remaining = r->content_len;
    unsigned started = seconds();
    while (err == ESP_OK && remaining) {
        if (seconds() - started >= 600) {
            ota_abort("Upload exceeded 10 minute safety limit");
            err = ESP_ERR_TIMEOUT;
            break;
        }
        size_t want = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        int n = httpd_req_recv(r, (char *)buffer, want);
        if (n <= 0) {
            ota_abort("Upload interrupted or timed out; previous firmware retained");
            err = ESP_FAIL;
            break;
        }
        err = ota_write(buffer, (size_t)n);
        remaining -= (size_t)n;
    }
    if (err == ESP_OK) err = ota_finish();
    if (err != ESP_OK) {
        ota_status_t ota;
        ota_get_status(&ota);
        cJSON *json = cJSON_CreateObject();
        if (json) cJSON_AddStringToObject(json, "error", ota.message);
        char *text = json ? cJSON_PrintUnformatted(json) : NULL;
        cJSON_Delete(json);
        httpd_resp_set_hdr(r, "Connection", "close");
        reply(r, "400 Bad Request", text ? text : "{\"error\":\"upgrade failed\"}");
        cJSON_free(text);
        atomic_store(&s->activity, seconds()); // Fresh idle window after failure.
        atomic_store(&s->gate, 0);
        return ESP_FAIL; // Discard any unread request bytes.
    }
    esp_err_t sent = reply(r, "200 OK", "{\"ok\":true,\"state\":\"rebooting\"}");
    // Even a lost response must not strand a successfully activated image.
    ota_response_sent();
    return sent;
}
static esp_err_t save_render_mode(httpd_req_t *r)
{
    session_t *s = session(r);
    cJSON *json = body_json(r);
    cJSON *mode = json ? cJSON_GetObjectItemCaseSensitive(json, "mode") : NULL;
    render_mode_t requested;
    // Exactly one field also rejects duplicate keys, unknown keys and aliases.
    bool valid = json && cJSON_GetArraySize(json) == 1 && cJSON_IsString(mode) &&
        render_mode_parse(mode->valuestring, &requested);
    cJSON_Delete(json);
    if (!valid) {
        httpd_resp_set_hdr(r, "Connection", "close");
        reply(r, "400 Bad Request", "{\"error\":\"expected exactly {mode: mono|linear|ink_match} as JSON\"}");
        return ESP_FAIL;
    }
    int expected = 0;
    if (ota_busy() || !atomic_compare_exchange_strong(&s->gate, &expected, 4))
        return reply(r, "409 Conflict", "{\"error\":\"upgrade, screen refresh, or session ending; retry when ready\"}");
    atomic_store(&s->activity, seconds());
    // HTTP never changes active mode or calls into UI. Gate 4 also prevents
    // idle exit/OTA from overtaking this accepted explicit user operation.
    esp_err_t err = render_settings_save(requested);
    atomic_store(&s->activity, seconds());
    atomic_store(&s->gate, 0);
    if (err != ESP_OK)
        return reply(r, "500 Internal Server Error", "{\"error\":\"could not save render setting; previous request retained\"}");
    return status_json(r); // Saved and active can differ until the owner tick.
}
static esp_err_t handle(httpd_req_t *r)
{
    if (!authorized(r)) return ESP_FAIL;
    session_t *s = session(r);
    if (atomic_load(&s->exiting)) {
        reply(r, "503 Service Unavailable", "{\"error\":\"session ending\"}");
        return ESP_FAIL;
    }
    if (r->method == HTTP_GET && !strcmp(r->uri, "/")) {
        // Navigating/reloading the UI is an explicit visit. Automatic polling
        // uses /api/status and never reaches this activity update.
        atomic_store(&s->activity, seconds());
        httpd_resp_set_type(r, "text/html; charset=utf-8");
        httpd_resp_set_hdr(r, "Cache-Control", "no-store");
        httpd_resp_set_hdr(r, "X-Content-Type-Options", "nosniff");
        httpd_resp_set_hdr(r, "Referrer-Policy", "no-referrer");
        httpd_resp_set_hdr(r, "Content-Security-Policy", "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'");
        return httpd_resp_send(r, html_start, html_end - html_start - 1);
    }
    if (r->method == HTTP_GET && !strcmp(r->uri, "/api/status")) return status_json(r);
    if (r->method == HTTP_POST && !strcmp(r->uri, "/api/ota")) return upload(r);
    if (r->method == HTTP_POST && !strcmp(r->uri, "/api/render-mode")) return save_render_mode(r);
    bool sync_route = !strcmp(r->uri, "/api/sync");
    bool exit_route = !strcmp(r->uri, "/api/exit");
    bool activity_route = !strcmp(r->uri, "/api/activity");
    if (r->method != HTTP_POST || !(sync_route || exit_route || activity_route))
        return unknown(r, HTTPD_404_NOT_FOUND);
    cJSON *json = body_json(r);
    bool valid = json && cJSON_GetArraySize(json) == 0;
    cJSON_Delete(json);
    if (!valid) {
        httpd_resp_set_hdr(r, "Connection", "close");
        reply(r, "400 Bad Request", "{\"error\":\"expected empty JSON object with valid content type and body size\"}");
        return ESP_FAIL;
    }
    if (ota_busy() || atomic_load(&s->gate) == 1)
        return reply(r, "409 Conflict", "{\"error\":\"upgrade in progress; do not disconnect power\"}");
    atomic_store(&s->activity, seconds());
    atomic_store(&s->syncing, sync_route);
    esp_err_t result = ESP_OK;
    if (sync_route) {
        xSemaphoreTake(s->network_mutex, portMAX_DELAY);
        result = network_sync_now();
        xSemaphoreGive(s->network_mutex);
    }
    atomic_store(&s->syncing, false);
    atomic_store(&s->activity, seconds());
    if (result != ESP_OK)
        return reply(r, "500 Internal Server Error", "{\"ok\":false,\"error\":\"operation failed\"}");
    esp_err_t sent = reply(r, "200 OK", "{\"ok\":true}");
    if (exit_route) {
        // Give the TCP response time to reach the client before stopping Wi-Fi.
        vTaskDelay(pdMS_TO_TICKS(500));
        atomic_store(&s->exiting, true);
    }
    return sent;
}

// Main task only. The parent's network poll hook uses app_poll, retains chosen
// actions and calls admin_note_activity; HTTP-side hooks never pump/draw UI.
static void show_management(uint8_t *frame, const session_t *s, const char *ip);
static bool cancelled(session_t *s, uint8_t *frame, const char *ip)
{
    if (ota_busy()) return false;
    int expected = 0;
    if (!atomic_compare_exchange_strong(&s->gate, &expected, 3)) return false;
    bool activity = false, selected = false;
    app_action_t action = app_poll(&activity, &selected);
    if (activity) atomic_store(&s->activity, seconds());
    atomic_store(&s->menu_open, app_ui_menu_open());
    if (action != APP_NONE && action != APP_LAN) s->selected = action;
    bool ending = s->selected != APP_NONE || atomic_load(&s->exiting) ||
        seconds() - atomic_load(&s->activity) >= 300;
    // An accepted save must survive an immediately following mode/exit choice.
    // Apply before leaving too; only the LAN redraw is skipped on that path.
    bool render_changed = render_settings_apply();
    if (!ending && render_changed) {
        // Render from source under the new mode, never reinterpret an old gray
        // frame. present preserves the open menu and updates its background.
        show_management(frame, s, ip);
        app_ui_refresh_menu();
        atomic_store(&s->menu_open, app_ui_menu_open());
    }
    // Claim the exit before releasing the UI gate. A newly arriving upload
    // must not overtake a mode choice that was already completed on screen.
    atomic_store(&s->gate, ending ? 2 : 0);
    return ending;
}
static bool authentication(session_t *s)
{
    char credentials[32];
    int n = snprintf(credentials, sizeof(credentials), "admin:%s", s->password);
    size_t encoded;
    memcpy(s->auth, "Basic ", 6);
    if (n < 0 || n >= (int)sizeof(credentials) ||
        mbedtls_base64_encode((unsigned char *)s->auth + 6, sizeof(s->auth) - 7,
                             &encoded, (unsigned char *)credentials, n) != 0) return false;
    s->auth[6 + encoded] = 0;
    memset(credentials, 0, sizeof(credentials));
    return true;
}
static esp_err_t start_server(httpd_handle_t *server, session_t *s)
{
    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    http.server_port = 80;
    http.global_user_ctx = s;
    http.global_user_ctx_free_fn = keep_session;
    http.uri_match_fn = httpd_uri_match_wildcard;
    http.stack_size = 12288; // 4 KiB upload buffer plus SDK image validation.
    http.recv_wait_timeout = 2;
    http.send_wait_timeout = 2;
    http.lru_purge_enable = true;
    esp_err_t err = httpd_start(server, &http);
    if (err != ESP_OK) return err;
    httpd_uri_t get = {.uri = "/*", .method = HTTP_GET, .handler = handle};
    httpd_uri_t post = {.uri = "/*", .method = HTTP_POST, .handler = handle};
    if ((err = httpd_register_uri_handler(*server, &get)) == ESP_OK)
        err = httpd_register_uri_handler(*server, &post);
    if (err == ESP_OK) err = httpd_register_err_handler(*server, HTTPD_404_NOT_FOUND, unknown);
    if (err == ESP_OK) err = httpd_register_err_handler(*server, HTTPD_405_METHOD_NOT_ALLOWED, unknown);
    if (err != ESP_OK) { httpd_stop(*server); *server = NULL; }
    return err;
}
static void show_management(uint8_t *frame, const session_t *s, const char *ip)
{
    ota_status_t ota;
    ota_get_status(&ota);
    if (ota_resume_management() || !frame) {
        char address[40], credential[40], firmware[64];
        snprintf(address, sizeof(address), "http://%s/", ip);
        snprintf(credential, sizeof(credential), "admin / %s", s->password);
        snprintf(firmware, sizeof(firmware), "固件版本: %s", esp_app_get_description()->version);
        const char *result = !strcmp(ota.state, "success") ? "升级成功，已恢复管理" :
                             !strcmp(ota.state, "rollback") ? "升级未生效，已回到旧固件" : "升级状态请查看网页";
        const char *lines[] = {result, firmware, address, credential,
                              "闲置五分钟返回时钟；长按中键选单"};
        app_ui_message("LAN 管理模式", lines, 5);
    } else {
        management_render(frame, s->password, ip);
        app_ui_present(frame, true);
    }
}
app_action_t admin_run(void)
{
    clock_management_info_t info = {0};
    httpd_handle_t server = NULL;
    uint8_t *frame = malloc(CLOCK_FRAME_SIZE);
    session_t s = {0};
    atomic_init(&s.activity, seconds());
    atomic_init(&s.exiting, false);
    atomic_init(&s.syncing, false);
    atomic_init(&s.gate, 0);
    atomic_init(&s.menu_open, app_ui_menu_open());
    s.network_mutex = xSemaphoreCreateMutex();
    active_session = &s;
    s.boot_id = esp_random();
    ota_get_password_override(s.password);
    app_ui_set_mode(APP_LAN);
    if (!s.network_mutex) {
        const char *lines[] = {"内存不足，无法启动 HTTP", "长按中键选单；闲置五分钟回时钟"};
        app_ui_message("LAN 管理不可用", lines, 2);
    }
    unsigned attempts = 0, next_try = seconds();
    bool upload_shown = false;
    char last_state[24] = "";
    for (;;) {
        bool upgrading = atomic_load(&s.gate) == 1 || ota_busy();
        if (upgrading) {
            app_ui_discard_input();
            ota_status_t ota;
            ota_get_status(&ota);
            if (!upload_shown || strcmp(last_state, ota.state)) {
                const char *lines[] = {"正在升级，请勿断电", "升级期间暂停选单和闲置退出", "网页可查看进度与结果"};
                app_ui_message("固件升级", lines, 3);
                snprintf(last_state, sizeof(last_state), "%s", ota.state);
                upload_shown = true;
            }
            if (ota_reboot_requested()) {
                // HTTP response has already been attempted and grace elapsed.
                esp_restart();
            }
        } else {
            if (upload_shown) {
                int expected = 0;
                if (atomic_compare_exchange_strong(&s.gate, &expected, 3)) {
                    const char *lines[] = {"本次升级失败，旧固件未替换", "管理仍可使用，网页查看错误详情", "闲置计时已重新开始：五分钟"};
                    app_ui_message("升级失败", lines, 3);
                    upload_shown = false;
                    atomic_store(&s.gate, 0);
                }
            }
            bool leave = cancelled(&s, frame, info.ip);
            if (atomic_load(&s.exiting)) leave = true;
            if (leave) {
                int expected = 0;
                if (atomic_load(&s.gate) == 2 ||
                    atomic_compare_exchange_strong(&s.gate, &expected, 2)) break;
                continue; // An upload won the race; it owns the session now.
            }
            // Serialize even getters against HTTP status/sync. Never block the
            // UI task behind NTP; simply retry this observation next tick.
            bool disconnected = false;
            if (s.network_mutex && !atomic_load(&s.syncing) &&
                xSemaphoreTake(s.network_mutex, 0) == pdTRUE) {
                disconnected = !network_connected();
                xSemaphoreGive(s.network_mutex);
            }
            if (disconnected) {
                if (server) {
                    network_cancel();
                    httpd_stop(server);
                    server = NULL;
                    network_stop();
                    network_clear_cancel();
                    next_try = seconds() + 10;
                    const char *lines[] = {"Wi-Fi 已断开", attempts < 3 ? "十秒后重新连接" : "自动重试次数已用完",
                        "长按中键选单；闲置五分钟回时钟"};
                    app_ui_message("LAN 暂不可用", lines, 3);
                }
                if (attempts < 3 && seconds() >= next_try) {
                    char count[48];
                    snprintf(count, sizeof(count), "连接尝试 %u / 3", ++attempts);
                    const char *lines[] = {count, "正在连接已保存的 Wi-Fi", "长按中键打开选单"};
                    app_ui_message("连接 LAN", lines, 3);
                    esp_err_t err = network_start_management(&info);
                    if (cancelled(&s, frame, info.ip)) continue;
                    if (err == ESP_OK) {
                        // Preserve password for the entire session, including
                        // reconnects and recovery after an OTA/rollback boot.
                        if (!s.password[0]) memcpy(s.password, info.password, sizeof(s.password));
                        if (!authentication(&s)) err = ESP_FAIL;
                        if (err == ESP_OK) err = start_server(&server, &s);
                    }
                    if (err == ESP_OK) {
                        int expected = 0;
                        if (atomic_compare_exchange_strong(&s.gate, &expected, 3)) {
                            show_management(frame, &s, info.ip);
                            atomic_store(&s.gate, 0);
                        }
                    } else {
                        network_stop();
                        next_try = seconds() + 10;
                        const char *failure[] = {"Wi-Fi 或 HTTP 启动失败", esp_err_to_name(err),
                            attempts < 3 ? "十秒后重试" : "三次尝试已用完，停止自动重试",
                            "仍在管理模式，长按中键选单", "五分钟无操作将回到时钟"};
                        app_ui_message("LAN 暂不可用", failure, 5);
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    atomic_store(&s.exiting, true);
    network_cancel();
    if (server) httpd_stop(server); // Join before releasing stack/session memory.
    network_stop();
    active_session = NULL;
    if (s.network_mutex) vSemaphoreDelete(s.network_mutex);
    ota_management_finished();
    free(frame);
    memset(s.password, 0, sizeof(s.password));
    memset(s.auth, 0, sizeof(s.auth));
    return s.selected != APP_NONE ? s.selected : APP_CLOCK;
}
