#include <stdio.h>
#include <stdlib.h>
#include "app.h"
#include "cmd.h"
#include "config.h"
#include "sdkconfig.h"
#if CONFIG_KTSN_WIFI
#include "net.h"
#endif
#include "automation.h"
#include "sky.h"
#include "io.h"
#include "render.h"
#include "settings.h"
#if CONFIG_KTSN_OTA
#include "ota.h"
#endif
#include "esp_log.h"
#include "esp_random.h"

void console_start(void);

static const char *TAG = "main";

channel_t g_channels[CHANNELS_MAX];
int       g_nchannels;
globals_t g_globals;
env_t     g_env = { .intensity = UNIT_ONE, .beatms = -1 };

/* Compile and run `src` once, in a temporary channel whose X Y Z O are GLOBAL_BUF_PIXELS long.
 * Meant for setting up the global buffers (palettes etc.); the temporary buffers are thrown away afterwards. */
int autoexec_run(const char *src, char *err, size_t errlen)
{
    channel_t tmp;
    if (channel_init(&tmp, GLOBAL_BUF_PIXELS, CHANNEL_SCRATCH, &g_env, &g_globals)) { snprintf(err, errlen, "out of memory"); return -1; }
    tmp.rng = esp_random() | 1u;
    int rc = channel_set_source(&tmp, src, err, errlen);
    if (rc == 0) {
        channel_render(&tmp);
        if (tmp.perf.faults) { snprintf(err, errlen, "run-time fault: %s", tmp.perf.last_err); rc = -1; }
    }
    channel_free(&tmp);
    return rc;
}

void app_main(void)
{
    cmd_init();
    ESP_ERROR_CHECK(settings_init());
    settings_apply_tz();
    io_apply();             /* outputs off as early as possible (relays) */
    g_env.intensity = g_set.intensity;
    ESP_ERROR_CHECK(globals_init(&g_globals, GLOBAL_BUF_PIXELS) ? ESP_ERR_NO_MEM : ESP_OK);

    automation_set_host();
    sky_tick();             /* -1s until the clock is set; autoexec may read them */
    char err[DSL_ERR_LEN];
    if (g_set.autoexec) {
        if (autoexec_run(g_set.autoexec, err, sizeof err)) ESP_LOGE(TAG, "autoexec: %s", err);
        else ESP_LOGI(TAG, "autoexec done");
    }

    for (int i = 0; i < g_set.nch; i++) {
        if (channel_init(&g_channels[i], g_set.ch[i].npix, CHANNEL_SCRATCH, &g_env, &g_globals)) {
            ESP_LOGE(TAG, "ch%d: not enough memory for %u px; channels from here on are disabled", i, g_set.ch[i].npix);
            break;
        }
        g_channels[i].rng = esp_random() | 1u;          /* xorshift state must be nonzero */
        g_nchannels = i + 1;
        char *src = settings_load_prog(i);
        if (src) {
            if (channel_set_source(&g_channels[i], src, err, sizeof err)) ESP_LOGE(TAG, "ch%d: stored program: %s", i, err);
            free(src);
        } else if (i == 0) {
            /* Nothing stored: dim green so a wired-up string shows life. */
            channel_set_source(&g_channels[0], "(fill (buf O) #00FF00) (scale (buf O) 0.1)", err, sizeof err);
        }
    }
    render_start();
    automation_init();      /* after the channels and the render task (intensity, play): runs once now, then every N s */
#if CONFIG_KTSN_WIFI
    net_wifi_apply();       /* connects if an SSID is stored */
#endif
#if CONFIG_KTSN_OTA
    ota_health_check();     /* a freshly updated image keeps itself only once it renders */
#endif
    console_start();
}
