#include "io.h"
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "settings.h"
#include <stddef.h>
#include "automation.h"
#include "app.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_continuous.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

_Static_assert(offsetof(io_cfg_t, pull) == IO_CFG_V1_SIZE, "the fields before pull must keep the stored 0.19.0 layout");

static const char *TAG = "io";
#define DUTY_BITS LEDC_TIMER_10_BIT                    /* 1024 steps; fine up to ~40 kHz on an 80 MHz clock */
#define DUTY_FULL (1u << 10)                         /* 2^bits = always high (1023 would leave a 1-step gap) */

static io_cfg_t s_cfg[IO_MAX];                         /* what is set up now (gpio -1 = nothing) */
static int8_t   s_ledc[IO_MAX];                        /* LEDC channel of a PWM output, -1 = none */
static int8_t   s_timer[IO_MAX];                       /* its LEDC timer */
static uint16_t s_val[IO_MAX];                         /* 0..256 */
static bool     s_ok[IO_MAX];                          /* set up and working */
static bool     s_init;
static uint16_t s_tfreq[SOC_LEDC_TIMER_NUM];           /* frequency of each timer, and how many outputs use it */
static uint8_t  s_tref[SOC_LEDC_TIMER_NUM];
static bool     s_chan_used[SOC_LEDC_CHANNEL_NUM];

/* Inputs: the inputs task polls digital pins every ms (debounce) and owns the ADC (continuous DMA, ADC1 only), which
 * it sets up again whenever s_adc_gen changes. Readers on other tasks only read these. */
static volatile bool     s_in_val[IO_MAX];             /* debounced, after invert */
static uint16_t          s_ring[IO_MAX][IO_SAMPLES_MAX];   /* analog readings 0..4095, one per ms */
static volatile uint16_t s_ring_at[IO_MAX], s_ring_n[IO_MAX];
static volatile uint32_t s_adc_gen;                    /* bumped by io_apply when the analog inputs change */
static TaskHandle_t      s_task;
#define READ_HZ    1000                                /* analog readings per second per input */
#define ADC_MAX    4095                                /* 12-bit on both chips (SOC_ADC_DIGI_MAX_BITWIDTH) */

static const char *const k_mode_names[IO_MODE_COUNT] = { "binary", "pwm", "input", "analog", "audio" };
static const char *const k_pull_names[3] = { "none", "up", "down" };
const char *io_mode_name(int m) { return m >= 0 && m < IO_MODE_COUNT ? k_mode_names[m] : "?"; }
const char *io_pull_name(int p) { return p >= 0 && p < 3 ? k_pull_names[p] : "?"; }
int io_mode_parse(const char *s) { for (int i = 0; i < IO_MODE_COUNT; i++) if (!strcmp(s, k_mode_names[i])) return i; return -1; }
int io_pull_parse(const char *s) { for (int i = 0; i < 3; i++) if (!strcmp(s, k_pull_names[i])) return i; return -1; }

const char *io_analog_pins(void)
{
#if CONFIG_IDF_TARGET_ESP32
    return "32..39";
#elif CONFIG_IDF_TARGET_ESP32C6
    return "0..6";
#else
    return "an ADC1 pin of this chip";
#endif
}

static bool adc1_channel(int gpio, adc_channel_t *ch)
{
    adc_unit_t unit;
    adc_channel_t c;
    if (adc_continuous_io_to_channel(gpio, &unit, &c) != ESP_OK || unit != ADC_UNIT_1) return false;
    if (ch) *ch = c;
    return true;
}

const char *io_check_name(const char *name)
{
    size_t n = strlen(name);
    if (!n) return NULL;
    if (n > IO_NAME_MAX) return "name: at most 15 characters";
    if (name[0] < 'a' || name[0] > 'z') return "name must start with a lower-case letter";
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return "name may only use a-z 0-9 - _";
    }
    return NULL;
}

const char *io_check(const io_cfg_t *io, int *bad)
{
    uint16_t freqs[SOC_LEDC_TIMER_NUM];
    int nfreq = 0, npwm = 0, naudio = 0;
    for (int i = 0; i < IO_MAX; i++) {
        const io_cfg_t *c = &io[i];
        *bad = i;
        if (c->gpio < 0) continue;
        if (c->mode >= IO_MODE_COUNT) return "mode is binary, pwm, input, analog or audio";
        const char *why = io_mode_is_output(c->mode) ? settings_check_gpio(c->gpio) : settings_check_gpio_in(c->gpio);
        if (why) return why;
        for (int ch = 0; ch < g_set.nch; ch++) if (g_set.ch[ch].gpio == c->gpio) return "that gpio drives an LED channel";
        for (int j = 0; j < i; j++) if (io[j].gpio == c->gpio) return "that gpio is already used by another slot";
        if ((why = io_check_name(c->name))) return why;
        for (int j = 0; j < i; j++) if (c->name[0] && io[j].gpio >= 0 && !strcmp(io[j].name, c->name)) return "another slot has that name";
        if (c->mode == IO_INPUT) {
            if (c->pull > IO_PULL_DOWN) return "pull is up, down or none";
            if (c->pull != IO_PULL_NONE && !GPIO_IS_VALID_OUTPUT_GPIO(c->gpio))   /* ESP32 34..39: input only, no pulls */
                return "this pin has no pull resistors: use pull=none and an external resistor";
            if (c->debounce > IO_DEBOUNCE_MAX) return "debounce must be 0..1000 ms";
        }
        if (io_mode_is_adc(c->mode) && !adc1_channel(c->gpio, NULL)) {
            static char msg[64];                       /* io_check runs under cmd_lock */
            snprintf(msg, sizeof msg, "%s inputs need an ADC1 pin (gpio %s)", c->mode == IO_AUDIO ? "audio" : "analog", io_analog_pins());
            return msg;
        }
        if (c->mode == IO_AUDIO && ++naudio > 1) return "only one audio input";
        if (c->mode == IO_PWM) {
            if (c->freq < IO_FREQ_MIN || c->freq > IO_FREQ_MAX) return "pwm frequency must be 1..20000 Hz";
            if (++npwm > SOC_LEDC_CHANNEL_NUM) return "too many pwm outputs for this chip";
            int k = 0;
            while (k < nfreq && freqs[k] != c->freq) k++;
            if (k == nfreq) { if (nfreq == SOC_LEDC_TIMER_NUM) return "too many different pwm frequencies (4 at most)"; freqs[nfreq++] = c->freq; }
        }
    }
    *bad = -1;
    return NULL;
}

static void drive(int n)
{
    const io_cfg_t *c = &s_cfg[n];
    if (!s_ok[n] || !io_mode_is_output(c->mode)) return;
    if (c->mode == IO_BINARY) gpio_set_level(c->gpio, (s_val[n] != 0) != (c->invert != 0));
    else {
        uint32_t duty = (uint32_t)s_val[n] * DUTY_FULL / 256u;
        if (c->invert) duty = DUTY_FULL - duty;
        /* not ledc_set_duty_and_update: that one needs the fade service (ledc_fade_func_install) */
        ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)s_ledc[n], duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)s_ledc[n]);
    }
}

/* Same hardware setup (the name doesn't matter): such an output is left alone, so it doesn't glitch. */
static bool same_hw(const io_cfg_t *a, const io_cfg_t *b)
{
    return a->gpio == b->gpio && a->mode == b->mode && a->invert == b->invert && (a->mode != IO_PWM || a->freq == b->freq)
        && (a->mode != IO_INPUT || (a->pull == b->pull && a->debounce == b->debounce && a->trigger == b->trigger));
}

static void release(int n)
{
    if (s_ledc[n] >= 0) {
        ledc_stop(LEDC_LOW_SPEED_MODE, (ledc_channel_t)s_ledc[n], s_cfg[n].invert ? 1 : 0);
        s_chan_used[s_ledc[n]] = false;
        if (s_tref[s_timer[n]] && !--s_tref[s_timer[n]]) ledc_timer_pause(LEDC_LOW_SPEED_MODE, (ledc_timer_t)s_timer[n]);
    }
    gpio_reset_pin(s_cfg[n].gpio);
    gpio_pullup_dis(s_cfg[n].gpio);                    /* left floating: no longer ours */
    s_ledc[n] = s_timer[n] = -1;
    s_ok[n] = false;
    s_cfg[n].gpio = -1;
}

static bool setup(int n, const io_cfg_t *c)
{
    if (io_mode_is_adc(c->mode)) {                     /* the inputs task sets up the ADC (and the pin) */
        s_ring_n[n] = s_ring_at[n] = 0;
        return true;
    }
    if (c->mode == IO_INPUT) {
        gpio_config_t g = { .pin_bit_mask = 1ULL << c->gpio, .mode = GPIO_MODE_INPUT,
                            .pull_up_en = c->pull == IO_PULL_UP, .pull_down_en = c->pull == IO_PULL_DOWN };
        if (gpio_config(&g) != ESP_OK) { ESP_LOGE(TAG, "input %d: gpio %d failed", n, c->gpio); return false; }
        s_in_val[n] = (gpio_get_level(c->gpio) != 0) != (c->invert != 0);   /* the state at setup: no trigger for it */
        return true;
    }
    if (c->mode == IO_BINARY) {
        gpio_config_t g = { .pin_bit_mask = 1ULL << c->gpio, .mode = GPIO_MODE_OUTPUT };
        gpio_set_level(c->gpio, c->invert ? 1 : 0);    /* off before it becomes an output */
        if (gpio_config(&g) != ESP_OK) { ESP_LOGE(TAG, "output %d: gpio %d failed", n, c->gpio); return false; }
        gpio_set_level(c->gpio, c->invert ? 1 : 0);
        return true;
    }
    int t = -1, freet = -1, ch = -1;
    for (int k = 0; k < SOC_LEDC_TIMER_NUM; k++) {
        if (s_tref[k] && s_tfreq[k] == c->freq) t = k;
        if (!s_tref[k] && freet < 0) freet = k;
    }
    for (int k = 0; k < SOC_LEDC_CHANNEL_NUM && ch < 0; k++) if (!s_chan_used[k]) ch = k;
    if (t < 0) {
        if (freet < 0 || ch < 0) { ESP_LOGE(TAG, "output %d: no pwm timer or channel left", n); return false; }
        ledc_timer_config_t tc = { .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = DUTY_BITS, .timer_num = (ledc_timer_t)freet,
                                   .freq_hz = c->freq, .clk_cfg = LEDC_AUTO_CLK };
        if (ledc_timer_config(&tc) != ESP_OK) { ESP_LOGE(TAG, "output %d: pwm %u Hz failed", n, c->freq); return false; }
        ledc_timer_resume(LEDC_LOW_SPEED_MODE, (ledc_timer_t)freet);
        t = freet;
        s_tfreq[t] = c->freq;
    }
    if (ch < 0) { ESP_LOGE(TAG, "output %d: no pwm channel left", n); return false; }
    ledc_channel_config_t cc = { .gpio_num = c->gpio, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = (ledc_channel_t)ch,
                                 .timer_sel = (ledc_timer_t)t, .duty = c->invert ? DUTY_FULL : 0, .hpoint = 0 };
    if (ledc_channel_config(&cc) != ESP_OK) { ESP_LOGE(TAG, "output %d: gpio %d pwm failed", n, c->gpio); return false; }
    s_tref[t]++;
    s_chan_used[ch] = true;
    s_ledc[n] = (int8_t)ch;
    s_timer[n] = (int8_t)t;
    return true;
}

/* ---- the inputs task ---- */

static adc_continuous_handle_t s_adc;
static int8_t   s_adc_slot[SOC_ADC_MAX_CHANNEL_NUM];   /* ADC1 channel -> slot, -1 = none */
static uint32_t s_acc[IO_MAX], s_acc_n[IO_MAX], s_decim;
static uint8_t  s_conv[512];                          /* one DMA frame at most (~5 ms of conversions, capped) */
static adc_continuous_data_t s_parsed[sizeof s_conv / SOC_ADC_DIGI_RESULT_BYTES];

/* the audio input (written by the inputs task) */
static int               s_audio_slot = -1;
static audio_t           s_audio;
static uint16_t          s_abuf[AUDIO_N];
static int               s_abuf_n;
static uint32_t          s_adc_hz, s_audio_decim;
static volatile uint32_t s_ovf, s_ovf0, s_blocks, s_us_sum, s_us_max, s_beat_at;
static volatile bool     s_beat_seen;

static bool IRAM_ATTR on_ovf(adc_continuous_handle_t h, const adc_continuous_evt_data_t *e, void *u)
{
    (void)h; (void)e; (void)u;
    s_ovf++;
    return false;
}

static void audio_publish_off(void)
{
    for (int b = 0; b < AUDIO_BANDS; b++) g_env.band[b] = 0;
    g_env.level = 0;
    g_env.beats = 0;
    s_beat_seen = false;
}

static void adc_stop(void)
{
    if (!s_adc) return;
    adc_continuous_stop(s_adc);
    adc_continuous_deinit(s_adc);
    s_adc = NULL;
}

/* (Re)start the ADC for the analog and audio inputs configured now. All of them are converted at the same rate (the
 * ADC goes round them in turn): analog inputs get READ_HZ readings, each the average of s_decim conversions; the
 * audio input AUDIO_RATE samples, each the average of a few conversions (a little anti-aliasing for free). */
static void adc_start(void)
{
    adc_stop();
    adc_digi_pattern_config_t pat[SOC_ADC_PATT_LEN_MAX];
    int np = 0, audio = -1;
    for (int i = 0; i < SOC_ADC_MAX_CHANNEL_NUM; i++) s_adc_slot[i] = -1;
    for (int i = 0; i < IO_MAX && np < SOC_ADC_PATT_LEN_MAX; i++) {
        adc_channel_t ch;
        if (s_cfg[i].gpio < 0 || !io_mode_is_adc(s_cfg[i].mode) || !s_ok[i] || !adc1_channel(s_cfg[i].gpio, &ch)) continue;
        pat[np++] = (adc_digi_pattern_config_t){ .atten = ADC_ATTEN_DB_12, .channel = ch, .unit = ADC_UNIT_1,
                                                 .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH };
        s_adc_slot[ch] = (int8_t)i;
        s_acc[i] = s_acc_n[i] = 0;
        if (s_cfg[i].mode == IO_AUDIO) audio = i;
    }
    s_audio_slot = -1;
    audio_publish_off();
    if (!np) return;
    uint32_t per;                                      /* conversions per second of each input */
    if (audio >= 0) {
        uint32_t k = 2;                                /* 16 kHz, 2 per sample, if the ADC allows */
        while (AUDIO_RATE * k * np < SOC_ADC_SAMPLE_FREQ_THRES_LOW) k++;
        while (k > 1 && AUDIO_RATE * k * np > SOC_ADC_SAMPLE_FREQ_THRES_HIGH) k--;
        per = AUDIO_RATE * k;
        s_audio_decim = k;
    } else {
        per = READ_HZ * 2;                             /* at least 2 conversions per reading */
        if (per * np < SOC_ADC_SAMPLE_FREQ_THRES_LOW) per = (SOC_ADC_SAMPLE_FREQ_THRES_LOW + np - 1) / np;
        per = (per + READ_HZ - 1) / READ_HZ * READ_HZ; /* a whole number of conversions per reading */
    }
    uint32_t total = per * np;
    s_decim = per / READ_HZ;
    uint32_t frame = total / 200 * SOC_ADC_DIGI_RESULT_BYTES;              /* ~5 ms of conversions */
    frame = (frame + SOC_ADC_DIGI_DATA_BYTES_PER_CONV - 1) / SOC_ADC_DIGI_DATA_BYTES_PER_CONV * SOC_ADC_DIGI_DATA_BYTES_PER_CONV;
    if (frame < SOC_ADC_DIGI_DATA_BYTES_PER_CONV * 4) frame = SOC_ADC_DIGI_DATA_BYTES_PER_CONV * 4;
    if (frame > sizeof s_conv) frame = sizeof s_conv;
    uint32_t store = frame * 4;                        /* room for ~40 ms, so a busy moment loses nothing */
    while (store < total / 25 * SOC_ADC_DIGI_RESULT_BYTES) store += frame;
    adc_continuous_handle_cfg_t hc = { .max_store_buf_size = store, .conv_frame_size = frame, .flags.flush_pool = 1 };
    adc_continuous_config_t cc = { .pattern_num = (uint32_t)np, .adc_pattern = pat, .sample_freq_hz = total,
                                   .conv_mode = ADC_CONV_SINGLE_UNIT_1 };
    adc_continuous_evt_cbs_t cbs = { .on_pool_ovf = on_ovf };
    esp_err_t e = adc_continuous_new_handle(&hc, &s_adc);
    if (e == ESP_OK) e = adc_continuous_config(s_adc, &cc);
    if (e == ESP_OK) e = adc_continuous_register_event_callbacks(s_adc, &cbs, NULL);
    if (e == ESP_OK) e = adc_continuous_start(s_adc);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "analog inputs: adc failed (%s)", esp_err_to_name(e));
        if (s_adc) { adc_continuous_deinit(s_adc); s_adc = NULL; }
        return;
    }
    s_adc_hz = total;
    if (audio >= 0) {
        audio_init(&s_audio);
        s_abuf_n = 0;
        s_audio_slot = audio;
        io_audio_stat_reset();
    }
    ESP_LOGI(TAG, "analog inputs: %d on the adc at %lu Hz, %lu conversions per reading%s", np, (unsigned long)total,
             (unsigned long)(per / READ_HZ), audio >= 0 ? " (audio: 8 kHz)" : "");
}

static void audio_sample(uint16_t v)
{
    s_abuf[s_abuf_n++] = v;
    if (s_abuf_n < AUDIO_N) return;
    s_abuf_n = 0;
    int64_t t0 = esp_timer_get_time();
    audio_block(&s_audio, s_abuf, (uint32_t)(t0 / 1000));
    uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    s_blocks++;
    s_us_sum += us;
    if (us > s_us_max) s_us_max = us;
    for (int b = 0; b < AUDIO_BANDS; b++) g_env.band[b] = s_audio.band[b];
    g_env.level = s_audio.level;
    g_env.beats = s_audio.beats;
    if (s_audio.beat) { s_beat_at = s_audio.beat_at; s_beat_seen = true; }
}

static void adc_poll(void)
{
    uint32_t len = 0;
    while (s_adc && adc_continuous_read(s_adc, s_conv, sizeof s_conv, &len, 0) == ESP_OK && len) {
        uint32_t n = 0;
        if (adc_continuous_parse_data(s_adc, s_conv, len, s_parsed, &n) != ESP_OK) break;
        for (uint32_t k = 0; k < n; k++) {
            const adc_continuous_data_t *d = &s_parsed[k];
            if (!d->valid || d->unit != ADC_UNIT_1 || d->channel >= SOC_ADC_MAX_CHANNEL_NUM) continue;
            int i = s_adc_slot[d->channel];
            if (i < 0) continue;
            s_acc[i] += d->raw_data > ADC_MAX ? ADC_MAX : d->raw_data;
            uint32_t decim = i == s_audio_slot ? s_audio_decim : s_decim;
            if (++s_acc_n[i] < decim) continue;
            uint16_t v = (uint16_t)((s_acc[i] + decim / 2) / decim);
            s_acc[i] = s_acc_n[i] = 0;
            if (i == s_audio_slot) { audio_sample(v); continue; }
            uint16_t at = s_ring_at[i];
            s_ring[i][at] = v;
            s_ring_at[i] = (uint16_t)((at + 1) % IO_SAMPLES_MAX);
            if (s_ring_n[i] < IO_SAMPLES_MAX) s_ring_n[i]++;
        }
    }
}

void io_audio_stat_reset(void)
{
    s_blocks = s_us_sum = s_us_max = 0;
    s_ovf0 = s_ovf;
}

void io_audio_stat(io_audio_stat_t *st)
{
    memset(st, 0, sizeof *st);
    st->slot = s_audio_slot;
    if (s_audio_slot < 0) return;
    st->adc_hz = s_adc_hz;
    st->per_sample = s_audio_decim;
    st->blocks = s_blocks;
    st->dropped = s_ovf - s_ovf0;
    st->us_avg = s_blocks ? s_us_sum / s_blocks : 0;
    st->us_max = s_us_max;
    for (int b = 0; b < AUDIO_BANDS; b++) st->band[b] = s_audio.band[b];   /* may tear mid-block: fine for a display */
    st->level = s_audio.level;
    st->beats = s_audio.beats;
    st->swing = s_audio.swing;
}

int32_t io_audio_beatms(void)
{
    if (s_audio_slot < 0 || !s_beat_seen) return -1;
    uint32_t d = (uint32_t)(esp_timer_get_time() / 1000) - s_beat_at;
    return d > 0x7FFFFFFF ? 0x7FFFFFFF : (int32_t)d;
}

static void inputs_task(void *arg)
{
    (void)arg;
    uint32_t gen = 0;
    uint16_t stable_ms[IO_MAX] = { 0 };
    for (;;) {
        if (gen != s_adc_gen) { gen = s_adc_gen; adc_start(); }
        adc_poll();
        bool kick = false;
        for (int i = 0; i < IO_MAX; i++) {             /* debounce: a new level must hold `debounce` ms */
            const io_cfg_t *c = &s_cfg[i];
            if (c->gpio < 0 || c->mode != IO_INPUT || !s_ok[i]) { stable_ms[i] = 0; continue; }
            bool v = (gpio_get_level(c->gpio) != 0) != (c->invert != 0);
            if (v == s_in_val[i]) { stable_ms[i] = 0; continue; }
            if (++stable_ms[i] < c->debounce) continue;
            s_in_val[i] = v;
            stable_ms[i] = 0;
            if (c->trigger) kick = true;
        }
        if (kick) automation_run_now();
        vTaskDelay(1);                                 /* 1 ms (CONFIG_FREERTOS_HZ 1000) */
    }
}

void io_apply(void)
{
    if (!s_init) {
        for (int i = 0; i < IO_MAX; i++) { s_cfg[i].gpio = -1; s_ledc[i] = s_timer[i] = -1; }
        s_init = true;
    }
    int bad;
    const char *why = io_check(g_set.io, &bad);
    if (why) { ESP_LOGE(TAG, "output %d: %s; outputs disabled", bad, why); }
    for (int i = 0; i < IO_MAX; i++)                   /* let go of what changed (first, so pins and channels free up) */
        if (s_cfg[i].gpio >= 0 && (why || !same_hw(&s_cfg[i], &g_set.io[i]))) release(i);
    if (why) return;
    for (int i = 0; i < IO_MAX; i++) {
        const io_cfg_t *c = &g_set.io[i];
        if (c->gpio < 0 || s_cfg[i].gpio >= 0) { if (c->gpio >= 0) memcpy(s_cfg[i].name, c->name, sizeof c->name); continue; }
        s_val[i] = 0;
        s_cfg[i] = *c;
        s_ok[i] = setup(i, c);
        if (!s_ok[i]) continue;
        if (io_mode_is_output(c->mode))
            ESP_LOGI(TAG, "output %d%s%s: gpio %d %s%s, off", i, c->name[0] ? " " : "", c->name, c->gpio,
                     io_mode_name(c->mode), c->invert ? " inverted" : "");
        else ESP_LOGI(TAG, "input %d%s%s: gpio %d %s%s", i, c->name[0] ? " " : "", c->name, c->gpio,
                      io_mode_name(c->mode), c->invert ? " inverted" : "");
    }
    /* the analog set (slot + pin) decides the ADC setup: tell the inputs task when it changed */
    static uint64_t s_analog_sig;
    uint64_t sig = 0;
    bool any_in = false;
    for (int i = 0; i < IO_MAX; i++) {
        if (s_cfg[i].gpio < 0 || !s_ok[i] || io_mode_is_output(s_cfg[i].mode)) continue;
        any_in = true;
        if (io_mode_is_adc(s_cfg[i].mode)) sig |= (uint64_t)((s_cfg[i].gpio + 1) | (s_cfg[i].mode == IO_AUDIO ? 0x80 : 0)) << (i * 8);
    }
    if (sig != s_analog_sig) { s_analog_sig = sig; s_adc_gen++; }
    if (any_in && !s_task) xTaskCreate(inputs_task, "inputs", 3072, NULL, tskIDLE_PRIORITY + 3, &s_task);
}

int io_on_gpio(int gpio)
{
    for (int i = 0; i < IO_MAX; i++) if (g_set.io[i].gpio >= 0 && g_set.io[i].gpio == gpio) return i;
    return -1;
}

int io_find(const char *name)
{
    for (int i = 0; i < IO_MAX; i++) if (g_set.io[i].gpio >= 0 && name[0] && !strcmp(g_set.io[i].name, name)) return i;
    return -1;
}

bool io_used(int n) { return n >= 0 && n < IO_MAX && g_set.io[n].gpio >= 0; }
int io_mode(int n) { return io_used(n) ? g_set.io[n].mode : -1; }

bool io_in_get(int n)
{
    return n >= 0 && n < IO_MAX && s_ok[n] && s_cfg[n].gpio >= 0 && s_cfg[n].mode == IO_INPUT && s_in_val[n];
}

uint32_t io_analog_get(int n, int samples)
{
    if (n < 0 || n >= IO_MAX || !s_ok[n] || s_cfg[n].gpio < 0 || s_cfg[n].mode != IO_ANALOG) return 0;
    int have = s_ring_n[n], at = s_ring_at[n];
    if (samples < 1) samples = 1;
    if (samples > have) samples = have;
    if (!samples) return 0;
    uint32_t sum = 0;
    for (int k = 1; k <= samples; k++) sum += s_ring[n][(at - k + IO_SAMPLES_MAX) % IO_SAMPLES_MAX];
    uint32_t v = (uint32_t)(((uint64_t)sum * IO_ANALOG_FULL + (uint64_t)samples * ADC_MAX / 2) / ((uint64_t)samples * ADC_MAX));
    return s_cfg[n].invert ? IO_ANALOG_FULL - v : v;
}

const char *io_set(int n, uint16_t v)
{
    if (!io_used(n)) return "no such output";
    if (!io_mode_is_output(g_set.io[n].mode)) return "that is an input";
    if (!s_ok[n]) return "output isn't working (see the log)";
    if (v > 256) v = 256;
    s_val[n] = g_set.io[n].mode == IO_BINARY ? (v ? 256 : 0) : v;
    drive(n);
    return NULL;
}

uint16_t io_get(int n)
{
    if (n < 0 || n >= IO_MAX || !s_ok[n] || s_cfg[n].gpio < 0) return 0;
    if (s_cfg[n].mode == IO_INPUT) return io_in_get(n) ? 256 : 0;
    if (s_cfg[n].mode == IO_ANALOG) return (uint16_t)((io_analog_get(n, 16) * 256u + IO_ANALOG_FULL / 2) / IO_ANALOG_FULL);
    if (s_cfg[n].mode == IO_AUDIO) return n == s_audio_slot ? (uint16_t)((g_env.level * 256u + AUDIO_ONE / 2) / AUDIO_ONE) : 0;
    return s_val[n];
}
bool io_working(int n) { return n >= 0 && n < IO_MAX && s_ok[n]; }
