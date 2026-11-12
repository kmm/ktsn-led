#include "progs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "automation.h"
#include "settings.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "progs";
#define PART "progs"
static const char *s_part = NVS_DEFAULT_PART_NAME;    /* where programs live: PART, or the settings NVS (old layout) */

bool progs_separate(void) { return strcmp(s_part, PART) == 0; }

static esp_err_t open_ns(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h) { return nvs_open_from_partition(s_part, ns, mode, h); }

/* malloc'd string value, or NULL */
static char *get_str(nvs_handle_t h, const char *key)
{
    size_t n = 0;
    if (nvs_get_str(h, key, NULL, &n) != ESP_OK) return NULL;
    char *s = malloc(n);
    if (s && nvs_get_str(h, key, s, &n) != ESP_OK) { free(s); s = NULL; }
    return s;
}

/* Programs kept in the settings NVS by older firmware (or before this board got the progs partition) move over once:
 * copy, commit, then erase the old keys. A key already in progs wins (never overwritten). */
static void migrate(void)
{
    nvs_handle_t from, to;
    if (nvs_open(PROGS_NS_SYS, NVS_READWRITE, &from) != ESP_OK) return;
    if (open_ns(PROGS_NS_SYS, NVS_READWRITE, &to) != ESP_OK) { nvs_close(from); return; }
    int moved = 0;
    for (int i = -1; i < 8; i++) {
        char k[8];
        if (i < 0) snprintf(k, sizeof k, "auto"); else snprintf(k, sizeof k, "prog%d", i);
        char *s = get_str(from, k);
        if (!s) continue;
        char *have = get_str(to, k);
        if (!have && nvs_set_str(to, k, s) != ESP_OK) { free(s); continue; }
        free(have);
        free(s);
        if (nvs_commit(to) == ESP_OK && nvs_erase_key(from, k) == ESP_OK) moved++;
    }
    if (moved) { nvs_commit(from); ESP_LOGI(TAG, "moved %d stored program(s) to the progs partition", moved); }
    nvs_close(to);
    nvs_close(from);
}

void progs_init(void)
{
    esp_err_t r = nvs_flash_init_partition(PART);
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "progs partition unusable (%s), erasing it", esp_err_to_name(r));
        if (nvs_flash_erase_partition(PART) == ESP_OK) r = nvs_flash_init_partition(PART);
    }
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "no progs partition (%s): programs stay in the settings NVS; flash the current layout over USB "
                      "for room for the library", esp_err_to_name(r));
        return;
    }
    s_part = PART;
    migrate();
}

char *progs_get(const char *ns, const char *key)
{
    nvs_handle_t h;
    if (open_ns(ns, NVS_READONLY, &h) != ESP_OK) return NULL;
    char *s = get_str(h, key);
    nvs_close(h);
    return s;
}

esp_err_t progs_set(const char *ns, const char *key, const char *src)
{
    nvs_handle_t h;
    esp_err_t r = open_ns(ns, NVS_READWRITE, &h);
    if (r != ESP_OK) return r;
    if (src && src[0]) r = nvs_set_str(h, key, src);
    else { r = nvs_erase_key(h, key); if (r == ESP_ERR_NVS_NOT_FOUND) r = ESP_OK; }
    if (r == ESP_OK) r = nvs_commit(h);
    nvs_close(h);
    return r;
}

void progs_list(const char *ns, void (*fn)(const char *key, size_t len, void *ctx), void *ctx)
{
    nvs_iterator_t it = NULL;
    esp_err_t r = nvs_entry_find(s_part, ns, NVS_TYPE_STR, &it);
    nvs_handle_t h;
    bool open = open_ns(ns, NVS_READONLY, &h) == ESP_OK;
    while (r == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        size_t n = 0;
        if (open) nvs_get_str(h, info.key, NULL, &n);
        fn(info.key, n ? n - 1 : 0, ctx);
        r = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    if (open) nvs_close(h);
}

void progs_stats(size_t *used, size_t *total)
{
    nvs_stats_t st = { 0 };
    nvs_get_stats(s_part, &st);
    *used = st.used_entries;
    *total = st.total_entries;
}

esp_err_t progs_erase_ns(const char *ns)
{
    nvs_handle_t h;
    esp_err_t r = open_ns(ns, NVS_READWRITE, &h);
    if (r != ESP_OK) return r == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : r;
    r = nvs_erase_all(h);
    if (r == ESP_OK) r = nvs_commit(h);
    nvs_close(h);
    return r;
}

volatile uint32_t g_lib_gen;

/* The change lib_put / lib_del are checking: what lib_source shows for s_over_name (NULL: deleted). */
static const char *s_over_name, *s_over_src;

char *lib_source(const char *name)
{
    if (s_over_name && !strcmp(name, s_over_name)) return s_over_src ? strdup(s_over_src) : NULL;
    return progs_get(PROGS_NS_LIB, name);
}

/* Compile what runs against the changed library (only programs that mention use). NULL if all of it compiles, with the
 * new channel programs in out[]; else the reason, and nothing to free. */
static const char *lib_recheck(program_t **out, char *err, size_t errlen)
{
    char e[DSL_ERR_LEN];
    const char *bad = NULL;
    char who[16] = "";
    memset(out, 0, CHANNELS_MAX * sizeof *out);
    for (int i = 0; i < g_nchannels && !bad; i++) {
        const char *src = g_channels[i].source;
        if (src && strstr(src, "use") && !(out[i] = dsl_compile(&g_channels[i], src, e, sizeof e))) { bad = e; snprintf(who, sizeof who, "ch%d", i); }
    }
    if (!bad && g_set.autoexec && g_nchannels && strstr(g_set.autoexec, "use")) {
        program_t *p = dsl_compile(&g_channels[0], g_set.autoexec, e, sizeof e);
        if (p) program_free(p); else { bad = e; strcpy(who, "autoexec"); }
    }
    if (!bad && g_set.automation && strstr(g_set.automation, "use") && automation_check(g_set.automation, e, sizeof e)) { bad = e; strcpy(who, "automation"); }
    if (!bad) return NULL;
    for (int i = 0; i < CHANNELS_MAX; i++) program_free(out[i]);
    snprintf(err, errlen, "%s would break: %.70s", who, bad);
    return err;
}

/* After the change is stored: what uses libraries runs the new functions. */
static void lib_publish(program_t **progs)
{
    for (int i = 0; i < CHANNELS_MAX; i++) if (progs[i]) channel_set_program(&g_channels[i], progs[i], g_channels[i].source);
    if (g_set.automation && strstr(g_set.automation, "use")) automation_reload();
}

esp_err_t lib_del(const char *name, char *err, size_t errlen)
{
    char *src = progs_get(PROGS_NS_LIB, name);
    if (!src) return ESP_ERR_NOT_FOUND;
    free(src);
    program_t *progs[CHANNELS_MAX];
    s_over_name = name; s_over_src = NULL;
    const char *why = lib_recheck(progs, err, errlen);
    s_over_name = NULL;
    if (why) return ESP_ERR_INVALID_STATE;
    esp_err_t r = progs_set(PROGS_NS_LIB, name, NULL);
    if (r != ESP_OK) { for (int i = 0; i < CHANNELS_MAX; i++) program_free(progs[i]); return r; }
    g_lib_gen++;
    lib_publish(progs);
    return r;
}

const char *lib_check_name(const char *name)
{
    size_t n = strlen(name);
    if (!n || n > LIB_NAME_MAX) return "name must be 1..15 characters";
    if (name[0] < 'a' || name[0] > 'z') return "name must start with a lower-case letter";
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return "name may only use a-z 0-9 - _";
    }
    return NULL;
}

const char *lib_put(const char *name, const char *src, char *err, size_t errlen)
{
    const char *why = lib_check_name(name);
    if (why) return why;
    if (!src[0]) return "program is empty (use lib rm to delete)";
    if (strlen(src) > SOURCE_MAX) return "program too long (max 3999 characters)";
    if (g_nchannels < 1) return "no channel to compile against";
    s_over_name = name; s_over_src = src;
    program_t *p = dsl_compile(&g_channels[0], src, err, errlen);   /* checked now, compiled again when it runs */
    program_t *progs[CHANNELS_MAX];
    const char *bad = !p ? err : lib_recheck(progs, err, errlen);
    s_over_name = NULL;
    program_free(p);
    if (bad) return bad;
    esp_err_t r = progs_set(PROGS_NS_LIB, name, src);
    if (r != ESP_OK) {
        for (int i = 0; i < CHANNELS_MAX; i++) program_free(progs[i]);
        snprintf(err, errlen, "could not store it: %s", r == ESP_ERR_NVS_NOT_ENOUGH_SPACE ? "program storage is full" : esp_err_to_name(r));
        return err;
    }
    g_lib_gen++;
    lib_publish(progs);
    return NULL;
}
