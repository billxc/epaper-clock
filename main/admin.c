#include "admin.h"
#include "network.h"
#include "rtc.h"
#include "display.h"
#include "epaper_port.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "cJSON.h"

extern const char html_start[] asm("_binary_admin_html_start");
extern const char html_end[] asm("_binary_admin_html_end");
// Immutable auth; all fields shared with the HTTP task are atomic.
typedef struct {
    char auth[64];
    atomic_uint activity;
    atomic_bool exiting, changed;
} session_t;
static unsigned seconds(void) { return (unsigned)(esp_timer_get_time() / 1000000); }
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
static bool wifi_config(cJSON *json, clock_wifi_config_t *config)
{
    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(json, "ssid");
    cJSON *pass = cJSON_GetObjectItemCaseSensitive(json, "password");
    if (cJSON_GetArraySize(json) != 2 || !cJSON_IsString(ssid) || !cJSON_IsString(pass)) return false;
    size_t s = strlen(ssid->valuestring), p = strlen(pass->valuestring);
    if (s < 1 || s > 32 || (p != 0 && (p < 8 || p > 64))) return false;
    for (size_t i = 0; i < p; ++i) {
        unsigned char c = pass->valuestring[i];
        if (p == 64 ? !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                       (c >= 'A' && c <= 'F')) : (c < 32 || c > 126)) return false;
    }
    memcpy(config->ssid, ssid->valuestring, s + 1);
    memcpy(config->password, pass->valuestring, p + 1);
    return true;
}
static esp_err_t status_json(httpd_req_t *r)
{
    struct tm now = {0};
    bool valid = rtc_read(&now);
    char time[32] = "", ip[16] = "";
    if (valid) strftime(time, sizeof(time), "%Y-%m-%d %H:%M:%S", &now);
    clock_wifi_config_t config;
    network_get_config(&config);
    network_station_ip(ip, sizeof(ip));
    cJSON *json = cJSON_CreateObject();
    if (!json) return reply(r, "500 Internal Server Error", "{\"error\":\"out of memory\"}");
    cJSON_AddStringToObject(json, "rtc_time", time);
    cJSON_AddBoolToObject(json, "rtc_valid", valid);
    cJSON_AddBoolToObject(json, "sta_connected", network_connected());
    cJSON_AddStringToObject(json, "sta_ip", ip);
    cJSON_AddStringToObject(json, "ssid", config.ssid);
    cJSON_AddNumberToObject(json, "last_sync", (double)network_last_sync());
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    esp_err_t result = reply(r, text ? "200 OK" : "500 Internal Server Error",
                            text ? text : "{\"error\":\"out of memory\"}");
    cJSON_free(text);
    return result;
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
        atomic_store(&s->activity, seconds());
        httpd_resp_set_type(r, "text/html; charset=utf-8");
        httpd_resp_set_hdr(r, "Cache-Control", "no-store");
        httpd_resp_set_hdr(r, "X-Content-Type-Options", "nosniff");
        httpd_resp_set_hdr(r, "Referrer-Policy", "no-referrer");
        httpd_resp_set_hdr(r, "Content-Security-Policy", "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'");
        return httpd_resp_send(r, html_start, html_end - html_start - 1);
    }
    if (r->method == HTTP_GET && !strcmp(r->uri, "/api/status")) return status_json(r);
    bool config_route = !strcmp(r->uri, "/api/config");
    bool sync_route = !strcmp(r->uri, "/api/sync");
    bool exit_route = !strcmp(r->uri, "/api/exit");
    if (r->method != HTTP_POST || !(config_route || sync_route || exit_route))
        return unknown(r, HTTPD_404_NOT_FOUND);
    cJSON *json = body_json(r);
    clock_wifi_config_t config = {0};
    bool valid = json && (config_route ? wifi_config(json, &config) : cJSON_GetArraySize(json) == 0);
    cJSON_Delete(json);
    if (!valid) {
        httpd_resp_set_hdr(r, "Connection", "close");
        reply(r, "400 Bad Request", "{\"error\":\"invalid JSON, content type, body size or Wi-Fi credentials\"}");
        return ESP_FAIL;
    }
    atomic_store(&s->activity, seconds());
    esp_err_t result = config_route ? network_save_config(&config) :
                       sync_route ? network_sync_now() : ESP_OK;
    if (result != ESP_OK)
        return reply(r, "500 Internal Server Error", "{\"ok\":false,\"error\":\"operation failed\"}");
    if (config_route) atomic_store(&s->changed, true);
    esp_err_t sent = reply(r, "200 OK", "{\"ok\":true}");
    if (!sync_route) {
        // Give the TCP response time to reach the client before stopping Wi-Fi.
        vTaskDelay(pdMS_TO_TICKS(500));
        atomic_store(&s->exiting, true);
    }
    return sent;
}

bool admin_run(void)
{
    clock_ap_info_t info = {0};
    httpd_handle_t server = NULL;
    uint8_t *frame = NULL;
    unsigned started = seconds();
    session_t s = {0};
    atomic_init(&s.activity, seconds());
    atomic_init(&s.exiting, false);
    atomic_init(&s.changed, false);
    if (network_start_management(&info) != ESP_OK) goto done;
    frame = malloc(CLOCK_FRAME_SIZE);
    if (!frame) goto done;
    EPD_Init();
    management_render(frame, info.ssid, info.password, info.ip);
    EPD_Display_Base(frame);
    EPD_Sleep();
    char credentials[32];
    int n = snprintf(credentials, sizeof(credentials), "admin:%s", info.password);
    size_t encoded;
    memcpy(s.auth, "Basic ", 6);
    if (n < 0 || n >= (int)sizeof(credentials) ||
        mbedtls_base64_encode((unsigned char *)s.auth + 6, sizeof(s.auth) - 7,
                              &encoded, (unsigned char *)credentials, n) != 0) goto done;
    s.auth[6 + encoded] = 0;
    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    http.server_port = 80;
    http.global_user_ctx = &s;
    http.global_user_ctx_free_fn = keep_session;
    http.uri_match_fn = httpd_uri_match_wildcard;
    http.stack_size = 8192;
    http.recv_wait_timeout = 2;
    http.send_wait_timeout = 2;
    http.lru_purge_enable = true;
    if (httpd_start(&server, &http) != ESP_OK) goto done;
    httpd_uri_t get = {.uri = "/*", .method = HTTP_GET, .handler = handle};
    httpd_uri_t post = {.uri = "/*", .method = HTTP_POST, .handler = handle};
    if (httpd_register_uri_handler(server, &get) != ESP_OK ||
        httpd_register_uri_handler(server, &post) != ESP_OK ||
        httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, unknown) != ESP_OK ||
        httpd_register_err_handler(server, HTTPD_405_METHOD_NOT_ALLOWED, unknown) != ESP_OK) goto done;
    atomic_store(&s.activity, seconds());
    while (!atomic_load(&s.exiting)) {
        unsigned activity = atomic_load(&s.activity), now = seconds();
        if (now - started >= 900 || now - activity >= 300) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
 done:
    atomic_store(&s.exiting, true);
    if (server) httpd_stop(server); // Joins the HTTP task before context/frame cleanup.
    network_stop();
    free(frame);
    return atomic_load(&s.changed);
}
