/* Serial JSON API for tools (the IDE): typing `api` on the serial console switches the line into machine mode.
 *
 *   request:   one JSON object per line   {"id": <any>, "op": "<op>", ...}
 *   response:  one line per request       0x1E {"id": <same>, "status": <http-style>, "body": {...}} \n
 *
 * Log output may still appear between responses, but never starts with 0x1E (ASCII record separator), so a client
 * reads only lines that do. Ops mirror the HTTP API (same code, api.c):
 *   ping | status | config | patch {"body": {...}} | prog {"ch": n, "src": "..."} | cmd {"line": "..."} |
 *   save | reboot | exit
 * A plain `exit` line or Ctrl-D also leaves, so a person who typed `api` by accident gets the prompt back.
 * No token: serial access is physical access, the same trust as the console itself. */
#include "serial_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "api.h"
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PROTO     1
#define RS        "\x1e"

/* One frame per response, written with a single fwrite so other tasks' log lines can't land inside it. */
static void send_frame(const cJSON *id, api_result_t res)
{
    cJSON *f = cJSON_CreateObject();
    cJSON_AddItemToObject(f, "id", id ? cJSON_Duplicate(id, true) : cJSON_CreateNull());
    cJSON_AddNumberToObject(f, "status", res.status);
    cJSON_AddItemToObject(f, "body", res.body ? res.body : cJSON_CreateObject());
    char *s = cJSON_PrintUnformatted(f);                  /* strings are escaped: no raw newlines inside */
    cJSON_Delete(f);
    if (s) {
        size_t n = strlen(s);
        char *out = malloc(n + 3);
        if (out) {
            out[0] = RS[0];
            memcpy(out + 1, s, n);
            out[n + 1] = '\n';
            fwrite(out, 1, n + 2, stdout);
            fflush(stdout);
            free(out);
        }
        cJSON_free(s);
    }
    if (res.after) res.after();
}

static api_result_t ping(void)
{
    cJSON *b = cJSON_CreateObject();
    cJSON_AddBoolToObject(b, "pong", true);
    cJSON_AddNumberToObject(b, "proto", PROTO);
    return (api_result_t){ .status = 200, .body = b };
}

/* Handle one request line. Returns false when the client asked to leave. */
static bool handle(const char *line, size_t len)
{
    cJSON *req = cJSON_ParseWithLength(line, len);
    if (!cJSON_IsObject(req)) { cJSON_Delete(req); send_frame(NULL, api_error(400, "request must be a JSON object")); return true; }
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id");
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(req, "op");
    const char *o = cJSON_IsString(op) ? op->valuestring : "";
    bool stay = true;
    api_result_t res;
    if (!strcmp(o, "ping"))        res = ping();
    else if (!strcmp(o, "status")) res = api_status();
    else if (!strcmp(o, "config")) res = api_get_config();
    else if (!strcmp(o, "patch"))  res = api_patch_config(cJSON_GetObjectItemCaseSensitive(req, "body"));
    else if (!strcmp(o, "save"))   res = api_save();
    else if (!strcmp(o, "reboot")) res = api_reboot();
    else if (!strcmp(o, "exit"))   { res = ping(); stay = false; }
    else if (!strcmp(o, "prog")) {
        const cJSON *ch = cJSON_GetObjectItemCaseSensitive(req, "ch"), *src = cJSON_GetObjectItemCaseSensitive(req, "src");
        if (!cJSON_IsNumber(ch) || !cJSON_IsString(src)) res = api_error(400, "prog needs \"ch\" (number) and \"src\" (string)");
        else res = api_put_prog((long)ch->valuedouble, src->valuestring);
    } else if (!strcmp(o, "cmd")) {
        const cJSON *l = cJSON_GetObjectItemCaseSensitive(req, "line");
        res = cJSON_IsString(l) ? api_cmd(l->valuestring) : api_error(400, "cmd needs \"line\" (string)");
    } else if (!strncmp(o, "lib", 3)) {                    /* lib, lib_get {name}, lib_put {name, src}, lib_del {name} */
        const cJSON *nm = cJSON_GetObjectItemCaseSensitive(req, "name"), *src = cJSON_GetObjectItemCaseSensitive(req, "src");
        const char *name = cJSON_IsString(nm) ? nm->valuestring : "";
        if (!strcmp(o, "lib")) res = api_lib_list();
        else if (!strcmp(o, "lib_get")) res = api_lib_get(name);
        else if (!strcmp(o, "lib_del")) res = api_lib_del(name);
        else if (!strcmp(o, "lib_put")) res = cJSON_IsString(src) ? api_lib_put(name, src->valuestring) : api_error(400, "lib_put needs \"name\" and \"src\"");
        else res = api_error(404, "unknown op (lib, lib_get, lib_put, lib_del)");
    } else res = api_error(404, "unknown op (ping status config patch prog cmd lib lib_get lib_put lib_del save reboot exit)");
    send_frame(id, res);
    cJSON_Delete(req);
    return stay;
}

static int cmd_api(int argc, char **argv)
{
    (void)argc; (void)argv;
    char *line = malloc(API_LINE_MAX);
    if (!line) { puts("out of memory"); return 1; }
    puts("api mode: one JSON request per line, responses start with 0x1E; `exit` to leave");
    cJSON *b = cJSON_CreateObject();
    cJSON_AddBoolToObject(b, "ready", true);
    cJSON_AddNumberToObject(b, "proto", PROTO);
    send_frame(NULL, (api_result_t){ .status = 200, .body = b });

    size_t len = 0;
    bool overflow = false, stay = true;
    while (stay) {
        int c = fgetc(stdin);
        if (c == EOF) { clearerr(stdin); vTaskDelay(1); continue; }   /* no data yet: poll every tick (1 ms) */
        if (c == 4) break;                                /* Ctrl-D */
        if (c == '\r') continue;
        if (c != '\n') {
            if (len < API_LINE_MAX - 1) line[len++] = (char)c; else overflow = true;
            continue;
        }
        line[len] = 0;
        if (overflow) send_frame(NULL, api_error(413, "request line too long"));
        else if (!strcmp(line, "exit")) stay = false;
        else if (len) stay = handle(line, len);
        len = 0;
        overflow = false;
    }
    free(line);
    puts("left api mode");
    return 0;
}

void serial_api_register(void)
{
    const esp_console_cmd_t c = {
        .command = "api",
        .help = "Machine-readable JSON mode for tools such as the IDE (serial only)",
        .func = cmd_api,                                  /* not via the command table: it must not hold cmd_lock */
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c));
}
