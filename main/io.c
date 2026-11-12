#include "io.h"
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "settings.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"

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
    int nfreq = 0, npwm = 0;
    for (int i = 0; i < IO_MAX; i++) {
        const io_cfg_t *c = &io[i];
        *bad = i;
        if (c->gpio < 0) continue;
        const char *why = settings_check_gpio(c->gpio);
        if (why) return why;
        for (int ch = 0; ch < g_set.nch; ch++) if (g_set.ch[ch].gpio == c->gpio) return "that gpio drives an LED channel";
        for (int j = 0; j < i; j++) if (io[j].gpio == c->gpio) return "that gpio is already an output";
        if ((why = io_check_name(c->name))) return why;
        for (int j = 0; j < i; j++) if (c->name[0] && io[j].gpio >= 0 && !strcmp(io[j].name, c->name)) return "another output has that name";
        if (c->mode > IO_PWM) return "mode is binary or pwm";
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
    if (!s_ok[n]) return;
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
    return a->gpio == b->gpio && a->mode == b->mode && a->invert == b->invert && (a->mode != IO_PWM || a->freq == b->freq);
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
        ESP_LOGI(TAG, "output %d%s%s: gpio %d %s%s, off", i, c->name[0] ? " " : "", c->name, c->gpio,
                 c->mode == IO_PWM ? "pwm" : "binary", c->invert ? " inverted" : "");
    }
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

const char *io_set(int n, uint16_t v)
{
    if (!io_used(n)) return "no such output";
    if (!s_ok[n]) return "output isn't working (see the log)";
    if (v > 256) v = 256;
    s_val[n] = g_set.io[n].mode == IO_BINARY ? (v ? 256 : 0) : v;
    drive(n);
    return NULL;
}

uint16_t io_get(int n) { return n >= 0 && n < IO_MAX && s_ok[n] ? s_val[n] : 0; }
bool io_working(int n) { return n >= 0 && n < IO_MAX && s_ok[n]; }
