#pragma once
#include "cJSON.h"

/* Transport-neutral JSON API: everything the HTTP API (http_api.c) and the serial API (serial_api.c) offer, so both
 * behave identically. Callers handle authentication and framing; these take cmd_lock themselves.
 * Each returns an HTTP-style status and a body (the caller cJSON_Delete()s it). `after`, if set, must be called once
 * the response has been sent: it applies changes that can cut the connection (WiFi) or restarts the board. */
typedef struct {
    int    status;               /* 200, 400, 404, 413, 500 */
    cJSON *body;
    void (*after)(void);
} api_result_t;

#define API_BODY_MAX   (12 * 1024)    /* a config patch may carry several programs */
#define API_OUTPUT_MAX (16 * 1024)    /* captured console output of api_cmd */
#define API_LINE_MAX   (API_BODY_MAX + 4096)   /* one serial request line; JSON escaping adds a little */

api_result_t api_status(void);
api_result_t api_get_config(void);
api_result_t api_patch_config(const cJSON *req);      /* all-or-nothing; see PATCH /api/config */
api_result_t api_put_prog(long ch, const char *src);
api_result_t api_cmd(const char *line);               /* body: {"result": <rc>, "output": "<text>"} */
api_result_t api_save(void);
api_result_t api_reboot(void);                        /* after = restart */
api_result_t api_error(int status, const char *msg);  /* {"error": msg} */
/* Program library (progs.h): list {programs: [{name, size}], used, total, separate}; get {name, src}; put; delete. */
api_result_t api_lib_list(void);
api_result_t api_lib_get(const char *name);
api_result_t api_lib_put(const char *name, const char *src);
api_result_t api_lib_del(const char *name);
