#include "render.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "app.h"
#include "config.h"
#include "led_out.h"
#include "settings.h"
#include "sky.h"
#include "dither.h"
#include "fader.h"
#include "gamma.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "render";
render_perf_t g_perf;
static TaskHandle_t s_task;
static esp_timer_handle_t s_timer;
static volatile uint8_t s_refresh = 1;                /* sends per rendered frame, as applied */
static uint8_t  s_gamma[CHANNELS_MAX][256];           /* 8-bit table (no dither) */
static uint16_t s_gamma16[CHANNELS_MAX][256];         /* 8.8 table (dither, spreading W) */
static volatile bool    s_gamma_on[CHANNELS_MAX];
static volatile uint8_t s_dither[CHANNELS_MAX];
static volatile uint16_t s_wmax[CHANNELS_MAX];        /* white limit 0..256 */
static volatile uint8_t  s_wmode[CHANNELS_MAX];
static uint16_t          s_wf[CHANNELS_MAX][3];       /* RGB light of full W, Q8 */
static volatile int s_progress = -1;                  /* firmware update: 0..1000, -1 = none */

/* Intensity faders (fader.h), [CHANNELS_MAX] = global. Changed from command tasks, stepped by the render task once per
 * rendered frame: a spinlock keeps cur/target/step consistent. */
static fader_t s_fade[CHANNELS_MAX + 1];
static portMUX_TYPE s_fade_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_cint16[CHANNELS_MAX];      /* per channel, 0..65536 (OUT_ONE), as the output uses it */
static volatile uint32_t s_gint16 = OUT_ONE;          /* global, 0..65536; g_env.intensity is the 0..256 view of it */

static fader_t *fader(int ch) { return ch == INTENSITY_GLOBAL ? &s_fade[CHANNELS_MAX] : ch >= 0 && ch < CHANNELS_MAX ? &s_fade[ch] : NULL; }

void render_set_intensity(int ch, uint16_t target, uint32_t fade_ms)
{
    fader_t *f = fader(ch);
    if (!f) return;
    if (target > UNIT_ONE) target = UNIT_ONE;
    if (fade_ms > FADE_MAX_MS) fade_ms = FADE_MAX_MS;
    int32_t frames = (int32_t)((uint64_t)fade_ms * FRAME_HZ / 1000);
    portENTER_CRITICAL(&s_fade_lock);
    fader_set(f, target, frames);
    portEXIT_CRITICAL(&s_fade_lock);
    if (frames <= 0) {                                    /* at once: visible on the next send even without a frame */
        if (ch == INTENSITY_GLOBAL) { g_env.intensity = target; s_gint16 = (uint32_t)target << 8; } else s_cint16[ch] = (uint32_t)target << 8;
    }
}

uint16_t render_intensity(int ch)        { fader_t *f = fader(ch); return f ? fader_value(f) : 0; }
uint16_t render_intensity_target(int ch) { fader_t *f = fader(ch); return f ? fader_target(f) : 0; }

/* One frame of every fade. */
static void fade_step(void)
{
    portENTER_CRITICAL(&s_fade_lock);
    for (int i = 0; i <= CHANNELS_MAX; i++) fader_step(&s_fade[i]);
    portEXIT_CRITICAL(&s_fade_lock);
    g_env.intensity = fader_value(&s_fade[CHANNELS_MAX]);
    s_gint16 = fader_value16(&s_fade[CHANNELS_MAX]);
    for (int i = 0; i < CHANNELS_MAX; i++) s_cint16[i] = fader_value16(&s_fade[i]);
}

void render_set_progress(int permille) { s_progress = permille; }

uint32_t render_stack_free(void) { return s_task ? (uint32_t)uxTaskGetStackHighWaterMark(s_task) : 0; }   /* bytes on ESP-IDF */

/* A dim bar over each string while a firmware update is written (programs are paused meanwhile). */
static void send_progress(int permille)
{
    static pixel_t *bar;
    static uint16_t cap;
    uint16_t need = 0;
    for (int i = 0; i < g_nchannels; i++) if (g_channels[i].npix > need) need = g_channels[i].npix;
    if (need > cap) { free(bar); bar = malloc((size_t)need * sizeof *bar); cap = bar ? need : 0; }
    if (!bar) return;
    for (int i = 0; i < g_nchannels; i++) {
        const out_map_t m = { .intensity16 = OUT_ONE, .gamma16 = s_gamma16[i], .wmax = UNIT_ONE };
        uint16_t n = g_channels[i].npix, lit = (uint16_t)((uint32_t)n * (uint32_t)permille / 1000);
        for (uint16_t k = 0; k < n; k++) bar[k] = k < lit ? 0x00003040u : 0x00020202u;
        led_out_write(i, bar, n, &m);
    }
    led_out_flush();
}
static SemaphoreHandle_t s_ready;

/* Called from command context while the render task runs: dither is switched off while the tables are rebuilt, and a
 * frame sent meanwhile may use a mix of old and new 8-bit entries (each a valid value): one frame at most, not visible. */
void render_output_update(int ch)
{
    if (ch < 0 || ch >= CHANNELS_MAX) return;
    s_dither[ch] = 0;
    s_gamma_on[ch] = g_set.gamma[ch] != GAMMA_OFF;
    if (s_gamma_on[ch]) gamma_lut(s_gamma[ch], g_set.gamma[ch]);
    gamma_lut16(s_gamma16[ch], g_set.gamma[ch]);          /* always: dither and spreading W need it */
    s_wmax[ch] = (uint16_t)((g_set.wmax[ch] * UNIT_ONE + 50) / 100);
    s_wmode[ch] = WMODE_DIRECT;                           /* off while the white point changes */
    wire_white_factors(g_set.wpoint[ch], g_set.wratio[ch], s_wf[ch]);
    s_wmode[ch] = g_set.wmode[ch];
    s_dither[ch] = g_set.dither[ch];
}

void perf_reset(void)
{
    memset(&g_perf, 0, sizeof g_perf);
    g_perf.t0_us = esp_timer_get_time();
    for (int i = 0; i < g_nchannels; i++) {
        memset(&g_channels[i].perf.render, 0, sizeof(stat_us_t));
        memset(&g_channels[i].perf.output, 0, sizeof(stat_us_t));
        g_channels[i].perf.faults = 0;
    }
}

static void env_tick(void)
{
    g_env.frame++;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t now = tv.tv_sec;
    struct tm tm;
    localtime_r(&now, &tm);
    g_env.ms = (uint16_t)(tv.tv_usec / 1000);
    g_env.weekday = (uint8_t)tm.tm_wday;
    g_env.timeset = (int64_t)now > SKY_CLOCK_SET_AFTER;
    g_env.year = (uint16_t)(tm.tm_year + 1900); g_env.month = (uint8_t)(tm.tm_mon + 1); g_env.day = (uint8_t)tm.tm_mday;
    g_env.doy = (uint16_t)(tm.tm_yday + 1);
    g_env.hour = (uint8_t)tm.tm_hour; g_env.minute = (uint8_t)tm.tm_min; g_env.second = (uint8_t)tm.tm_sec;
}

static void render_task(void *arg)
{
    (void)arg;
    /* LED outputs are created here so their RMT interrupts live on this core, away from WiFi on core 0.
     * A channel that fails (bad GPIO, out of RMT channels) is logged and stays dark; it never stops the boot. */
    for (int i = 0; i < g_nchannels; i++) {
        const char *bad = settings_check_gpio(g_set.ch[i].gpio);   /* e.g. a flash pin on another chip: never touch it */
        if (bad) { ESP_LOGE(TAG, "ch%d: gpio %d %s, channel disabled (fix with: chan %d <gpio> <npix>)", i, g_set.ch[i].gpio, bad, i); continue; }
        esp_err_t r = led_out_init(i, g_set.ch[i].gpio, g_set.ch[i].npix, g_set.ch[i].wire);
        if (r == ESP_OK) ESP_LOGI(TAG, "ch%d: gpio %d, %u px, wire %s", i, g_set.ch[i].gpio, g_set.ch[i].npix, led_out_wire(i));
        else ESP_LOGE(TAG, "ch%d: output on gpio %d failed (%s), channel disabled%s", i, g_set.ch[i].gpio, esp_err_to_name(r),
                      r == ESP_ERR_NOT_FOUND ? " -- out of RMT channels/memory: fewer channels or smaller "
                                               "KTSN_RMT_MEM_SYMBOLS (menuconfig KTSN LED)" : "");
    }
    esp_err_t r = led_out_start();                        /* PARLIO builds its single unit here; no-op for RMT */
    if (r != ESP_OK) ESP_LOGE(TAG, "LED output failed to start (%s): all channels dark", esp_err_to_name(r));
    xSemaphoreGive(s_ready);
    uint32_t refresh = 1, since = 1;                     /* since: ticks since the last render */
    for (;;) {
        /* Returns the number of ticks since the last wait; >1 means we overran a tick. */
        uint32_t ticks = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (ticks > 1) g_perf.missed += ticks - 1;

        /* The timer ticks `refresh` times per frame: render once every `refresh` ticks (counting missed ticks, so the
         * frame rate stays 60 Hz when a slow render swallows a tick), send on every tick. Between renders buffer O
         * doesn't change (only this task writes it), so no copy is needed; each send gets the next dither step. */
        uint8_t n = s_refresh;
        if (n != refresh) { refresh = n; since = refresh; }
        int progress = s_progress;
        if (progress >= 0) { send_progress(progress); continue; }
        int64_t t0 = esp_timer_get_time();
        since += ticks;
        bool render = since >= refresh;
        if (render) since %= refresh;
        if (render) {
            fade_step();
            env_tick();
            for (int i = 0; i < g_nchannels; i++) {
                int64_t a = esp_timer_get_time();
                g_channels[i].deadline_us = t0 + LOOP_LIMIT_US > a + LOOP_MIN_US ? t0 + LOOP_LIMIT_US : a + LOOP_MIN_US;
                channel_render(&g_channels[i]);
                stat_record(&g_channels[i].perf.render, (uint32_t)(esp_timer_get_time() - a));
            }
        }
        for (int i = 0; i < g_nchannels; i++) {
            channel_t *ch = &g_channels[i];
            int64_t b = esp_timer_get_time();
            const out_map_t m = { .intensity16 = intensity16_mul(s_gint16, s_cint16[i]), .gamma = s_gamma_on[i] ? s_gamma[i] : NULL,
                                  .gamma16 = s_gamma16[i], .dither = s_dither[i], .frame = g_perf.sends, .wmax = s_wmax[i],
                                  .wmode = s_wmode[i], .wf = { s_wf[i][0], s_wf[i][1], s_wf[i][2] } };
            led_out_write(i, ch->buf[BUF_O], ch->npix, &m);
            stat_record(&ch->perf.output, (uint32_t)(esp_timer_get_time() - b));
        }
        int64_t f0 = esp_timer_get_time();
        led_out_flush();
        stat_record(&g_perf.flush, (uint32_t)(esp_timer_get_time() - f0));
        g_perf.sends++;
        if (render) { stat_record(&g_perf.frame, (uint32_t)(esp_timer_get_time() - t0)); g_perf.frames++; }
    }
}

static uint32_t period_us(unsigned refresh) { return 1000000u / (FRAME_HZ * refresh); }

const char *render_check_output(unsigned refresh, bool parallel, char *why, size_t cap)
{
    if (refresh < 1 || refresh > REFRESH_MAX) { snprintf(why, cap, "refresh must be 1..%d (x %d Hz)", REFRESH_MAX, FRAME_HZ); return why; }
    if (refresh == 1) return NULL;                       /* the classic rate: always allowed */
    bool together = parallel || !strcmp(led_out_driver(), "parlio");
    uint32_t total = 0;
    for (int i = 0; i < g_nchannels; i++) {
        uint32_t t = led_out_send_us(i);
        total = together ? (t > total ? t : total) : total + t;
    }
    uint32_t budget = period_us(refresh) * 9 / 10;       /* leave room for rendering and jitter */
    if (total <= budget) return NULL;
    snprintf(why, cap, "too fast for these strings: sending takes ~%lu us, %u Hz leaves %lu us%s", (unsigned long)total,
             FRAME_HZ * refresh, (unsigned long)budget, !together && g_nchannels > 1 ? " (try output parallel on)" : "");
    return why;
}

void render_output_config(void)
{
    led_out_set_parallel(g_set.out_parallel);
    uint8_t r = g_set.refresh >= 1 && g_set.refresh <= REFRESH_MAX ? g_set.refresh : 1;
    if (r == s_refresh) return;
    s_refresh = r;
    if (s_timer) esp_timer_restart(s_timer, period_us(r));
}

/* The timer callback only wakes the render task; no rendering happens in the timer context. */
static void frame_timer_cb(void *arg)
{
    (void)arg;
    xTaskNotifyGive(s_task);
}

void render_start(void)
{
    perf_reset();
    dsl_set_clock(esp_timer_get_time);
    for (int i = 0; i < CHANNELS_MAX; i++) render_output_update(i);
    render_set_intensity(INTENSITY_GLOBAL, g_env.intensity, 0);
    for (int i = 0; i < CHANNELS_MAX; i++) render_set_intensity(i, g_set.cintens[i], 0);
    /* Priority above the console task; on dual-core chips pinned to core 1, away from WiFi (core 0). */
    s_ready = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(render_task, "render", RENDER_STACK, NULL, configMAX_PRIORITIES - 3, &s_task,
                            portNUM_PROCESSORS > 1 ? 1 : tskNO_AFFINITY);
    xSemaphoreTake(s_ready, portMAX_DELAY);              /* outputs exist before the first frame */
    const esp_timer_create_args_t ta = { .callback = frame_timer_cb, .name = "frame" };
    char why[120];
    if (render_check_output(g_set.refresh, g_set.out_parallel, why, sizeof why)) {     /* layout changed since saved */
        ESP_LOGW(TAG, "stored refresh x%u: %s; using x1", g_set.refresh, why);
        g_set.refresh = 1;
    }
    led_out_set_parallel(g_set.out_parallel);
    s_refresh = g_set.refresh;
    ESP_ERROR_CHECK(esp_timer_create(&ta, &s_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_timer, period_us(s_refresh)));
    ESP_LOGI(TAG, "render task started at %d Hz, output %u Hz%s", FRAME_HZ, FRAME_HZ * s_refresh,
             g_set.out_parallel ? ", strings in parallel" : "");
}
