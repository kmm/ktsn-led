#include "automation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "app.h"
#include "cmd.h"
#include "config.h"
#include "io.h"
#include "progs.h"
#include "render.h"
#include "sky.h"
#include "settings.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "automation";
#define STOP_SRC   "(fill (buf O) #000000)"           /* what (stop ch) puts on a channel */
#define STOPPED    "\x01stop"                         /* s_playing marker for a stopped channel */

static channel_t s_ch;                                 /* the automation program's own channel (ctx AUTOMATION, 1 px) */
static bool s_ready;
static volatile bool s_run_now;
static uint32_t s_runs;
static int64_t s_last_run = -1;
static bool s_last_run_is_time;

/* What automation last put on each channel, so (play ch name) every few seconds doesn't recompile: skipped while the
 * channel still runs exactly that (same source pointer: nobody else set a program since) and the library is unchanged. */
static char        s_playing[CHANNELS_MAX][LIB_NAME_MAX + 1];
static const char *s_set_src[CHANNELS_MAX];
static uint32_t    s_lib_gen[CHANNELS_MAX];
static char        s_err[DSL_ERR_LEN + 32];           /* fault text handed back to the evaluator (one run at a time) */

static bool unchanged(int ch, const char *name)
{
    return !strcmp(s_playing[ch], name) && g_channels[ch].source == s_set_src[ch] && s_lib_gen[ch] == g_lib_gen;
}
static void remember(int ch, const char *name)
{
    snprintf(s_playing[ch], sizeof s_playing[ch], "%s", name);
    s_set_src[ch] = g_channels[ch].source;
    s_lib_gen[ch] = g_lib_gen;
}

static int         host_nch(void) { return g_nchannels; }
static uint16_t    host_get(int ch) { return render_intensity(ch < 0 ? INTENSITY_GLOBAL : ch); }
static void        host_set(int ch, uint16_t v, uint32_t fade_ms) { render_set_intensity(ch < 0 ? INTENSITY_GLOBAL : ch, v, fade_ms); }
static bool        host_exists(const char *name) { char *s = progs_get(PROGS_NS_LIB, name); free(s); return s != NULL; }

static const char *host_play(int ch, const char *name)
{
    if (unchanged(ch, name)) return NULL;
    char *src = progs_get(PROGS_NS_LIB, name);
    if (!src) { snprintf(s_err, sizeof s_err, "play: no library program '%s'", name); return s_err; }
    if (g_channels[ch].source && !strcmp(src, g_channels[ch].source)) { free(src); remember(ch, name); return NULL; }
    char err[DSL_ERR_LEN];
    int rc = channel_set_source(&g_channels[ch], src, err, sizeof err);
    free(src);
    if (rc) { snprintf(s_err, sizeof s_err, "play %s: %s", name, err); return s_err; }
    remember(ch, name);
    ESP_LOGI(TAG, "ch%d: playing %s", ch, name);
    return NULL;
}

static const char *host_stop(int ch)
{
    if (unchanged(ch, STOPPED)) return NULL;
    char err[DSL_ERR_LEN];
    if (channel_set_source(&g_channels[ch], STOP_SRC, err, sizeof err)) { snprintf(s_err, sizeof s_err, "stop: %s", err); return s_err; }
    remember(ch, STOPPED);
    return NULL;
}

/* What automation put on ch, while nobody else has set a program there since (play-pick / play-library keep it). */
static const char *host_playing(int ch)
{
    if (ch < 0 || ch >= CHANNELS_MAX || !s_playing[ch][0] || !strcmp(s_playing[ch], STOPPED) || g_channels[ch].source != s_set_src[ch]) return NULL;
    return s_playing[ch];
}

/* Library programs whose names start with prefix, in name order (NVS lists them in no particular order). */
typedef struct { const char *prefix; size_t plen; char (*names)[LIB_NAME_MAX + 1]; int n, cap; } match_t;
static void match_one(const char *key, size_t len, void *ctx)
{
    (void)len;
    match_t *m = ctx;
    if (strncmp(key, m->prefix, m->plen)) return;
    if (m->n < m->cap) snprintf(m->names[m->n], sizeof m->names[0], "%s", key);
    m->n++;
}
static int cmp_name(const void *a, const void *b) { return strcmp(a, b); }
static int host_lib_match(const char *prefix, int k, char name[16])
{
    match_t m = { .prefix = prefix, .plen = strlen(prefix), .cap = k < 0 ? 0 : DSL_LIST_MAX };
    if (k >= 0 && !(m.names = malloc(DSL_LIST_MAX * sizeof *m.names))) return 0;
    progs_list(PROGS_NS_LIB, match_one, &m);
    if (k >= 0) {
        int have = m.n < m.cap ? m.n : m.cap;
        qsort(m.names, (size_t)have, sizeof *m.names, cmp_name);
        if (k < have) snprintf(name, 16, "%s", m.names[k]);
        else m.n = 0;
        free(m.names);
    }
    return m.n;
}

static const char *host_io_set(int n, uint16_t v)
{
    const char *why = io_set(n, v);
    if (why) { snprintf(s_err, sizeof s_err, "io-set %d: %s", n, why); return s_err; }
    return NULL;
}

_Static_assert((int)IO_BINARY == (int)DSL_IO_BINARY && (int)IO_PWM == (int)DSL_IO_PWM && (int)IO_INPUT == (int)DSL_IO_INPUT && (int)IO_ANALOG == (int)DSL_IO_ANALOG && (int)IO_AUDIO == (int)DSL_IO_AUDIO,
               "io.h modes must match the core's DSL_IO_*");
_Static_assert(IO_ANALOG_FULL == 65536 && IO_SAMPLES_MAX == DSL_ANALOG_SAMPLES_MAX, "io.h analog scale must match the core");
static const dsl_host_t k_host = { host_nch, host_get, host_set, host_play, host_stop, host_exists, io_find, io_used, io_get, host_io_set, lib_source, NULL,
                                   io_mode, io_in_get, io_analog_get, host_playing, host_lib_match };

void automation_set_host(void) { dsl_set_host(&k_host); }

static void run_once(void)
{
    cmd_lock();
    if (s_ready && g_set.automation) {
        channel_render(&s_ch);
        s_runs++;
        time_t t = time(NULL);
        s_last_run_is_time = t > 1700000000;               /* after 2023: the clock has been set (NTP or env time) */
        s_last_run = s_last_run_is_time ? (int64_t)t : esp_timer_get_time() / 1000000;
    }
    cmd_unlock();
}

static void task(void *arg)
{
    (void)arg;
    int64_t last_slot = -1;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        sky_tick();                                        /* low priority, so its few ms never cost a frame */
        uint32_t every = g_set.auto_every >= 1 && g_set.auto_every <= AUTO_EVERY_MAX ? g_set.auto_every : AUTO_EVERY_DEFAULT;
        time_t t = time(NULL);
        int64_t secs = t > 1700000000 ? (int64_t)t : esp_timer_get_time() / 1000000;
        int64_t slot = secs / every;                       /* a new slot = a multiple of `every` seconds was crossed */
        if (s_run_now || slot != last_slot) {
            last_slot = slot;
            s_run_now = false;
            run_once();
        }
    }
}

const char *automation_check(const char *src, char *err, size_t errlen)
{
    if (!s_ready) { snprintf(err, errlen, "automation isn't running"); return err; }
    program_t *p = dsl_compile(&s_ch, src, err, errlen);
    if (!p) return err;
    program_free(p);
    return NULL;
}

const char *automation_set(const char *src, char *err, size_t errlen)
{
    if (!s_ready) { snprintf(err, errlen, "automation isn't running"); return err; }
    if (!src || !src[0]) {
        channel_set_source(&s_ch, "", err, errlen);         /* an empty program: nothing to run */
        free(g_set.automation);
        g_set.automation = NULL;
        return NULL;
    }
    if (strlen(src) > SOURCE_MAX) { snprintf(err, errlen, "program too long (max %d characters)", SOURCE_MAX); return err; }
    char *copy = strdup(src);
    if (!copy) { snprintf(err, errlen, "out of memory"); return err; }
    if (channel_set_source(&s_ch, src, err, errlen)) { free(copy); return err; }
    free(g_set.automation);
    g_set.automation = copy;
    s_ch.perf.faults = 0;
    s_ch.perf.last_err = NULL;
    s_run_now = true;
    return NULL;
}

void automation_run_now(void) { s_run_now = true; }

void automation_reload(void)
{
    char err[DSL_ERR_LEN];
    if (s_ready && g_set.automation && channel_set_source(&s_ch, g_set.automation, err, sizeof err)) ESP_LOGE(TAG, "reload: %s", err);
}

void automation_get_status(automation_status_t *st)
{
    st->active = g_set.automation != NULL;
    st->runs = s_runs;
    st->faults = s_ch.perf.faults;
    st->last_err = s_ch.perf.last_err;
    st->last_run = s_last_run;
    st->last_run_is_time = s_last_run_is_time;
}

void automation_init(void)
{
    dsl_set_host(&k_host);
    if (channel_init(&s_ch, 1, CHANNEL_SCRATCH, &g_env, &g_globals)) { ESP_LOGE(TAG, "out of memory: automation disabled"); return; }
    s_ch.ctx = DSL_CTX_AUTOMATION;
    s_ch.rng = esp_random() | 1u;
    s_ready = true;
    if (g_set.automation) {
        char err[DSL_ERR_LEN];
        if (channel_set_source(&s_ch, g_set.automation, err, sizeof err)) {
            ESP_LOGE(TAG, "stored automation program: %s (not running)", err);
            free(g_set.automation);
            g_set.automation = NULL;                       /* kept in flash until the next save; fix it with `automation` */
        } else ESP_LOGI(TAG, "running every %u s", g_set.auto_every);
    }
    xTaskCreate(task, "automation", AUTOMATION_STACK, NULL, tskIDLE_PRIORITY + 2, NULL);
}
