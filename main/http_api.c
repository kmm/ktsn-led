/* HTTP/JSON API: a thin transport over api.c. Every request needs `Authorization: Bearer <token>`; with no token set,
 * everything is refused. Plain HTTP: trusted LAN only.
 *
 *   GET   /api/status        fps, timing, faults, wifi, time, free heap
 *   GET   /api/config        running configuration (wifi password and token are never returned)
 *   PATCH /api/config        partial update, all-or-nothing: everything is validated (programs compiled) first
 *   PUT   /api/prog/<n>      program source as the body (text/plain, multi-line, up to SOURCE_MAX chars)
 *   POST  /api/cmd           one console command line as the body; its output is the response (text/plain)
 *   POST  /api/save          persist everything (same as the `save` command)
 *   POST  /api/reboot
 *   GET   /api/lib           the program library: {programs: [{name, size}], used, total, separate}
 *   GET   /api/lib/<name>    {name, src};  PUT (source as the body, text/plain): store;  DELETE: remove
 *   POST  /api/ota           firmware image (application/octet-stream, build/ktsn-led.bin) -> written, then restart
 *                            (CONFIG_KTSN_OTA; the board keeps the new image only if it runs: see ota.c)
 *
 * CORS: every response allows any origin (the IDE runs on localhost, not on the board), and OPTIONS preflights are
 * answered without a token, as browsers send none. Access is still controlled by the token on the real request. */
#include "http_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "api.h"
#include "cmd.h"
#include "settings.h"
#if CONFIG_KTSN_OTA
#include "ota.h"
#endif
#include "sdkconfig.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "http";

static void cors(httpd_req_t *r)
{
    httpd_resp_set_hdr(r, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(r, "Access-Control-Expose-Headers", "X-Ktsn-Result, X-Ledctl-Result");
}

static const char *status_line(int status)
{
    switch (status) {
    case 200: return "200 OK";
    case 204: return "204 No Content";
    case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    case 403: return "403 Forbidden";
    case 404: return "404 Not Found";
    case 413: return "413 Payload Too Large";
    default:  return "500 Internal Server Error";
    }
}

static esp_err_t send_result(httpd_req_t *r, api_result_t res)
{
    char *s = cJSON_PrintUnformatted(res.body);
    cJSON_Delete(res.body);
    cors(r);
    httpd_resp_set_status(r, status_line(res.status));
    httpd_resp_set_type(r, "application/json");
    esp_err_t e = httpd_resp_sendstr(r, s ? s : "{}");
    cJSON_free(s);
    if (res.after) res.after();
    return e;
}

/* True if the request carries the right token; otherwise the error response has been sent. */
static bool authorized(httpd_req_t *r)
{
    cmd_lock();
    bool set = g_set.token[0] != 0;
    cmd_unlock();
    if (!set) {
        send_result(r, api_error(403, "network access is disabled until a token is set on the serial console: token <secret>"));
        return false;
    }
    char hdr[96];
    if (httpd_req_get_hdr_value_str(r, "Authorization", hdr, sizeof hdr) == ESP_OK &&
        !strncmp(hdr, "Bearer ", 7) && settings_token_ok(hdr + 7))
        return true;
    ESP_LOGW(TAG, "rejected %s %s: bad or missing token", http_method_str(r->method), r->uri);
    vTaskDelay(pdMS_TO_TICKS(500));                      /* slow down guessing */
    httpd_resp_set_hdr(r, "WWW-Authenticate", "Bearer");
    send_result(r, api_error(401, "missing or wrong token (send: Authorization: Bearer <token>)"));
    return false;
}

/* Read the whole body into a malloc'd, NUL-terminated buffer. NULL if it failed (response already sent). */
static char *read_body(httpd_req_t *r, size_t max, size_t *len)
{
    if (r->content_len > max) { send_result(r, api_error(413, "body too large")); return NULL; }
    char *buf = malloc(r->content_len + 1);
    if (!buf) { send_result(r, api_error(500, "out of memory")); return NULL; }
    size_t got = 0;
    while (got < r->content_len) {
        int n = httpd_req_recv(r, buf + got, r->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) { free(buf); send_result(r, api_error(400, "could not read body")); return NULL; }
        got += (size_t)n;
    }
    buf[got] = 0;
    *len = got;
    return buf;
}

static esp_err_t h_status(httpd_req_t *r)     { return authorized(r) ? send_result(r, api_status()) : ESP_OK; }
static esp_err_t h_get_config(httpd_req_t *r) { return authorized(r) ? send_result(r, api_get_config()) : ESP_OK; }
static esp_err_t h_save(httpd_req_t *r)       { return authorized(r) ? send_result(r, api_save()) : ESP_OK; }
static esp_err_t h_reboot(httpd_req_t *r)     { return authorized(r) ? send_result(r, api_reboot()) : ESP_OK; }

static esp_err_t h_patch_config(httpd_req_t *r)
{
    if (!authorized(r)) return ESP_OK;
    size_t len;
    char *body = read_body(r, API_BODY_MAX, &len);
    if (!body) return ESP_OK;
    cJSON *root = cJSON_ParseWithLength(body, len);
    free(body);
    api_result_t res = api_patch_config(root);
    cJSON_Delete(root);
    return send_result(r, res);
}

static esp_err_t h_put_prog(httpd_req_t *r)
{
    if (!authorized(r)) return ESP_OK;
    const char *num = r->uri + strlen("/api/prog/");
    char *end;
    long n = strtol(num, &end, 10);
    if (!*num || (*end && *end != '?')) return send_result(r, api_error(404, "use /api/prog/<channel number>"));
    size_t len;
    char *src = read_body(r, SOURCE_MAX, &len);
    if (!src) return ESP_OK;
    api_result_t res = api_put_prog(n, src);
    free(src);
    return send_result(r, res);
}

/* Plain-text response (the command's output), result code in X-Ktsn-Result (and the same in X-Ledctl-Result, its
 * name before the rename, for older clients). */
static esp_err_t h_cmd(httpd_req_t *r)
{
    if (!authorized(r)) return ESP_OK;
    size_t len;
    char *line = read_body(r, CMD_LINE_MAX - 1, &len);
    if (!line) return ESP_OK;
    api_result_t res = api_cmd(line);
    free(line);
    const cJSON *out = cJSON_GetObjectItemCaseSensitive(res.body, "output");
    if (!cJSON_IsString(out)) return send_result(r, res);  /* an error object, e.g. reboot refused */
    const cJSON *rc = cJSON_GetObjectItemCaseSensitive(res.body, "result");
    char rcs[12];
    snprintf(rcs, sizeof rcs, "%d", cJSON_IsNumber(rc) ? rc->valueint : -1);
    cors(r);
    httpd_resp_set_hdr(r, "X-Ktsn-Result", rcs);
    httpd_resp_set_hdr(r, "X-Ledctl-Result", rcs);
    httpd_resp_set_status(r, status_line(res.status));
    httpd_resp_set_type(r, "text/plain");
    esp_err_t e = httpd_resp_sendstr(r, out->valuestring);
    cJSON_Delete(res.body);
    return e;
}

/* /api/lib/<name>[?...] -> name in buf; false (404 sent) if there is none. */
static bool lib_name(httpd_req_t *r, char *buf, size_t cap)
{
    const char *p = r->uri + strlen("/api/lib/");
    size_t n = strcspn(p, "?");
    if (!n || n >= cap) { send_result(r, api_error(404, "use /api/lib/<name>")); return false; }
    memcpy(buf, p, n); buf[n] = 0;
    return true;
}

static esp_err_t h_lib_list(httpd_req_t *r) { return authorized(r) ? send_result(r, api_lib_list()) : ESP_OK; }

static esp_err_t h_lib_get(httpd_req_t *r)
{
    char name[24];
    if (!authorized(r) || !lib_name(r, name, sizeof name)) return ESP_OK;
    return send_result(r, api_lib_get(name));
}

static esp_err_t h_lib_del(httpd_req_t *r)
{
    char name[24];
    if (!authorized(r) || !lib_name(r, name, sizeof name)) return ESP_OK;
    return send_result(r, api_lib_del(name));
}

static esp_err_t h_lib_put(httpd_req_t *r)
{
    char name[24];
    if (!authorized(r) || !lib_name(r, name, sizeof name)) return ESP_OK;
    size_t len;
    char *src = read_body(r, SOURCE_MAX, &len);
    if (!src) return ESP_OK;
    api_result_t res = api_lib_put(name, src);
    free(src);
    return send_result(r, res);
}

#if CONFIG_KTSN_OTA
/* Streams the body into the inactive app slot in 4 KB pieces (no image-sized buffer). On success the board restarts
 * after the response; on any failure nothing changes and the old firmware keeps running. */
static esp_err_t h_ota(httpd_req_t *r)
{
    if (!authorized(r)) return ESP_OK;
    if (r->content_len == 0) return send_result(r, api_error(400, "send the image (build/ktsn-led.bin) as the body"));
    const char *why = ota_begin(r->content_len);
    if (why) return send_result(r, api_error(400, why));
    char *buf = malloc(4096);
    if (!buf) { ota_abort(); return send_result(r, api_error(500, "out of memory")); }
    size_t got = 0;
    int timeouts = 0;
    while (!why && got < r->content_len) {
        int n = httpd_req_recv(r, buf, r->content_len - got < 4096 ? r->content_len - got : 4096);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 6) continue;
        if (n <= 0) { ota_abort(); why = "upload interrupted"; break; }
        timeouts = 0;
        got += (size_t)n;
        why = ota_write(buf, (size_t)n);
    }
    free(buf);
    if (!why) why = ota_end();
    if (why) return send_result(r, api_error(400, why));
    api_result_t res = api_reboot();                      /* {"ok": true}, restart after the response */
    cJSON_AddStringToObject(res.body, "version", ota_new_version());
    return send_result(r, res);
}
#endif

/* CORS preflight: no token (browsers send none here). Also answers Chrome's private-network preflight. */
static esp_err_t h_options(httpd_req_t *r)
{
    cors(r);
    httpd_resp_set_hdr(r, "Access-Control-Allow-Methods", "GET, PATCH, PUT, POST, DELETE, OPTIONS");
    httpd_resp_set_hdr(r, "Access-Control-Allow-Headers", "Authorization, Content-Type");
    httpd_resp_set_hdr(r, "Access-Control-Allow-Private-Network", "true");
    httpd_resp_set_hdr(r, "Access-Control-Max-Age", "600");
    httpd_resp_set_status(r, "204 No Content");
    return httpd_resp_send(r, NULL, 0);
}

static esp_err_t h_not_found(httpd_req_t *r, httpd_err_code_t code)
{
    (void)code;
    send_result(r, api_error(404, "unknown endpoint; see /api/status, /api/config, /api/prog/<n>, /api/cmd, /api/save, /api/reboot, /api/lib, /api/ota"));
    return ESP_FAIL;
}

void http_api_start(void)
{
    static httpd_handle_t s_server;
    if (s_server) return;
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.server_port = CONFIG_KTSN_HTTP_PORT;
    c.ctrl_port = CONFIG_KTSN_HTTP_PORT + 1;
    c.stack_size = 8192;                                  /* handlers compile programs */
    c.core_id = 0;                                        /* with WiFi; render runs on core 1 */
    c.max_uri_handlers = 16;
    c.uri_match_fn = httpd_uri_match_wildcard;
    c.lru_purge_enable = true;
    if (httpd_start(&s_server, &c) != ESP_OK) { ESP_LOGE(TAG, "can't start HTTP server on port %d", c.server_port); return; }
    static const httpd_uri_t routes[] = {
        { .uri = "/api/status",  .method = HTTP_GET,     .handler = h_status },
        { .uri = "/api/config",  .method = HTTP_GET,     .handler = h_get_config },
        { .uri = "/api/config",  .method = HTTP_PATCH,   .handler = h_patch_config },
        { .uri = "/api/prog/*",  .method = HTTP_PUT,     .handler = h_put_prog },
        { .uri = "/api/cmd",     .method = HTTP_POST,    .handler = h_cmd },
        { .uri = "/api/save",    .method = HTTP_POST,    .handler = h_save },
        { .uri = "/api/reboot",  .method = HTTP_POST,    .handler = h_reboot },
        { .uri = "/api/lib",     .method = HTTP_GET,     .handler = h_lib_list },
        { .uri = "/api/lib/*",   .method = HTTP_GET,     .handler = h_lib_get },
        { .uri = "/api/lib/*",   .method = HTTP_PUT,     .handler = h_lib_put },
        { .uri = "/api/lib/*",   .method = HTTP_DELETE,  .handler = h_lib_del },
#if CONFIG_KTSN_OTA
        { .uri = "/api/ota",     .method = HTTP_POST,    .handler = h_ota },
#endif
        { .uri = "/api/*",       .method = HTTP_OPTIONS, .handler = h_options },
    };
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++) httpd_register_uri_handler(s_server, &routes[i]);
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, h_not_found);
    ESP_LOGI(TAG, "HTTP API on port %d", c.server_port);
}
