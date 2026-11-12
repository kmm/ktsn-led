/* Persistent settings in NVS, namespace "ledctl" (the project's old name, kept so
 * boards keep their settings across the rename to KTSN LED).
 * Keys: lat, lon, autoevery, nch, gpio<N>, npix<N>, wire<N>, cint<N>, gam<N>, dith<N>, wmax<N>, wmod<N>, wpt<N>, wrat<N>, refresh, outpar, intens, ssid, pass, ntp, tz, token, host.
 * Program text (prog<N>, auto, automat) lives in the progs partition (progs.c), or here on boards with an older partition table.
 * Loading never fails hard: missing or invalid values fall back to defaults, so a bad save can't brick the boot. */
#include "settings.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "app.h"
#include "dither.h"
#include "gamma.h"
#include "automation.h"
#include "progs.h"
#include "render.h"
#include "driver/gpio.h"
#include "esp_private/esp_gpio_reserve.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define NS "ledctl"           /* not renamed: see above */
static const char *TAG = "settings";
static const led_chan_cfg_t k_default_ch[] = LED_CHANNELS_INIT;

static void default_io(io_cfg_t *io)
{
    memset(io, 0, IO_MAX * sizeof *io);
    for (int i = 0; i < IO_MAX; i++) { io[i].gpio = -1; io[i].freq = IO_PWM_DEFAULT_HZ; io[i].debounce = IO_DEBOUNCE_DEFAULT; }
#ifdef IO_DEFAULTS_INIT
    static const io_cfg_t k[] = IO_DEFAULTS_INIT;
    _Static_assert(sizeof k / sizeof k[0] <= IO_MAX, "too many default outputs");
    memcpy(io, k, sizeof k);
#endif
}
settings_t g_set;

static void set_defaults(void)
{
    free(g_set.autoexec);
    free(g_set.automation);
    memset(&g_set, 0, sizeof g_set);
    g_set.auto_every = AUTO_EVERY_DEFAULT;
    g_set.nch = sizeof k_default_ch / sizeof k_default_ch[0];
    memcpy(g_set.ch, k_default_ch, sizeof k_default_ch);
    g_set.intensity = UNIT_ONE;
    for (int i = 0; i < CHANNELS_MAX; i++) {
        g_set.gamma[i] = GAMMA_OFF; g_set.wmax[i] = 100;
        g_set.wpoint[i] = WPOINT_DEFAULT; g_set.wratio[i] = 100; g_set.cintens[i] = UNIT_ONE;
    }
    g_set.refresh = 1;
    default_io(g_set.io);
    snprintf(g_set.ntp, sizeof g_set.ntp, "%s", DEFAULT_NTP_SERVER);
    snprintf(g_set.tz, sizeof g_set.tz, "%s", DEFAULT_TZ);
    snprintf(g_set.hostname, sizeof g_set.hostname, "%s", DEFAULT_HOSTNAME);
}

/* malloc'd string value of key, or NULL. */
static char *get_str(nvs_handle_t h, const char *key)
{
    size_t n = 0;
    if (nvs_get_str(h, key, NULL, &n) != ESP_OK) return NULL;
    char *s = malloc(n);
    if (s && nvs_get_str(h, key, s, &n) != ESP_OK) { free(s); s = NULL; }
    return s;
}

/* Copy key into dst if stored and it fits; otherwise leave dst (the default) alone. */
static void get_str_into(nvs_handle_t h, const char *key, char *dst, size_t cap)
{
    char *s = get_str(h, key);
    if (s && strlen(s) < cap) strcpy(dst, s);
    else if (s) ESP_LOGW(TAG, "stored '%s' too long, ignored", key);
    free(s);
}

static void load(nvs_handle_t h)
{
    uint8_t nch;
    if (nvs_get_u8(h, "nch", &nch) == ESP_OK) {
        led_chan_cfg_t ch[CHANNELS_MAX];
        bool ok = nch >= 1 && nch <= CHANNELS_MAX;
        for (int i = 0; ok && i < nch; i++) {
            char k[8]; int8_t gpio; uint16_t npix;
            snprintf(k, sizeof k, "gpio%d", i); ok = nvs_get_i8(h, k, &gpio) == ESP_OK;
            snprintf(k, sizeof k, "npix%d", i); ok = ok && nvs_get_u16(h, k, &npix) == ESP_OK && npix >= 1 && npix <= NPIX_MAX;
            ch[i].gpio = gpio; ch[i].npix = npix;
            snprintf(ch[i].wire, sizeof ch[i].wire, "%s", WIRE_DEFAULT);       /* not stored = the default */
            snprintf(k, sizeof k, "wire%d", i);
            char *ws = get_str(h, k);
            wire_t wt;
            if (ws && settings_check_wire(ws, &wt, ch[i].wire)) { ESP_LOGW(TAG, "ch%d: stored wire format '%s' is invalid, using %s", i, ws, WIRE_DEFAULT); snprintf(ch[i].wire, sizeof ch[i].wire, "%s", WIRE_DEFAULT); }
            free(ws);
        }
        if (ok) { g_set.nch = nch; memcpy(g_set.ch, ch, (size_t)nch * sizeof ch[0]); }
        else ESP_LOGW(TAG, "stored channel layout is invalid, using defaults");
    }
    uint16_t v;
    if (nvs_get_u16(h, "intens", &v) == ESP_OK && v <= UNIT_ONE) g_set.intensity = v;
    for (int i = 0; i < CHANNELS_MAX; i++) {
        char k[8];
        snprintf(k, sizeof k, "gam%d", i);
        if (nvs_get_u16(h, k, &v) == ESP_OK && v >= GAMMA_MIN && v <= GAMMA_MAX) g_set.gamma[i] = v;
        uint8_t d;
        snprintf(k, sizeof k, "dith%d", i);
        if (nvs_get_u8(h, k, &d) == ESP_OK && d <= DITHER_MAX) g_set.dither[i] = d;
        snprintf(k, sizeof k, "wmax%d", i);
        if (nvs_get_u8(h, k, &d) == ESP_OK && d <= 100) g_set.wmax[i] = d;
        snprintf(k, sizeof k, "wmod%d", i);
        if (nvs_get_u8(h, k, &d) == ESP_OK && d <= WMODE_BOOST) g_set.wmode[i] = d;
        uint32_t pt;
        snprintf(k, sizeof k, "wpt%d", i);
        if (nvs_get_u32(h, k, &pt) == ESP_OK && pt <= 0xFFFFFFu && pt) g_set.wpoint[i] = pt;
        snprintf(k, sizeof k, "wrat%d", i);
        if (nvs_get_u16(h, k, &v) == ESP_OK && v >= 10 && v <= 400) g_set.wratio[i] = v;
        snprintf(k, sizeof k, "cint%d", i);
        if (nvs_get_u16(h, k, &v) == ESP_OK && v <= UNIT_ONE) g_set.cintens[i] = v;
    }
    if (nvs_get_u16(h, "autoevery", &v) == ESP_OK && v >= 1 && v <= AUTO_EVERY_MAX) g_set.auto_every = v;
    uint8_t u8;
    if (nvs_get_u8(h, "refresh", &u8) == ESP_OK && u8 >= 1 && u8 <= REFRESH_MAX) g_set.refresh = u8;
    if (nvs_get_u8(h, "outpar", &u8) == ESP_OK) g_set.out_parallel = u8 != 0;
    io_cfg_t io[IO_MAX];
    size_t iol = sizeof io;
    if (nvs_get_blob(h, "io", io, &iol) == ESP_OK) {
        int bad;
        bool ok = iol == sizeof io;
        if (iol == IO_MAX * IO_CFG_V1_SIZE) {          /* saved before inputs existed (0.19.0 and older): outputs only */
            uint8_t raw[IO_MAX * IO_CFG_V1_SIZE];
            memcpy(raw, io, sizeof raw);
            memset(io, 0, sizeof io);
            for (int i = 0; i < IO_MAX; i++) { memcpy(&io[i], raw + i * IO_CFG_V1_SIZE, IO_CFG_V1_SIZE); io[i].debounce = IO_DEBOUNCE_DEFAULT; }
            ok = true;
        }
        for (int i = 0; ok && i < IO_MAX; i++) { io[i].name[IO_NAME_MAX] = 0; if (io[i].gpio < -1) ok = false; }
        if (ok && !io_check(io, &bad)) memcpy(g_set.io, io, sizeof io);
        else ESP_LOGW(TAG, "stored outputs are invalid, using defaults");
    }
    get_str_into(h, "ssid", g_set.ssid, sizeof g_set.ssid);
    get_str_into(h, "pass", g_set.pass, sizeof g_set.pass);
    get_str_into(h, "ntp", g_set.ntp, sizeof g_set.ntp);
    get_str_into(h, "tz", g_set.tz, sizeof g_set.tz);
    int32_t la, lo;
    if (nvs_get_i32(h, "lat", &la) == ESP_OK && nvs_get_i32(h, "lon", &lo) == ESP_OK && !settings_check_loc(la / 1e5, lo / 1e5)) {
        g_set.has_loc = true; g_set.lat = la; g_set.lon = lo;
    }
    get_str_into(h, "token", g_set.token, sizeof g_set.token);
    get_str_into(h, "host", g_set.hostname, sizeof g_set.hostname);
    if (settings_check_hostname(g_set.hostname)) snprintf(g_set.hostname, sizeof g_set.hostname, "%s", DEFAULT_HOSTNAME);
}

/* Pins reserved before any of our drivers ran (flash, PSRAM, ...). Our own LED channels and outputs reserve their
 * pins too (gpio_config, RMT, LEDC), so checking esp_gpio_is_reserved later would refuse a pin we drive ourselves. */
static uint64_t s_sys_reserved;

esp_err_t settings_init(void)
{
    for (int g = 0; g < SOC_GPIO_PIN_COUNT; g++) if (esp_gpio_is_reserved(BIT64(g))) s_sys_reserved |= BIT64(g);
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition unusable (%s), erasing it", esp_err_to_name(r));
        ESP_ERROR_CHECK(nvs_flash_erase());
        r = nvs_flash_init();
    }
    if (r != ESP_OK) return r;
    progs_init();                                         /* program text partition; moves old programs over once */
    set_defaults();
    g_set.autoexec = progs_get(PROGS_NS_SYS, "auto");
    g_set.automation = progs_get(PROGS_NS_SYS, "automat");
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) { ESP_LOGI(TAG, "nothing stored yet, using defaults"); return ESP_OK; }
    load(h);
    nvs_close(h);
    return ESP_OK;
}

char *settings_load_prog(int ch)
{
    char k[8];
    snprintf(k, sizeof k, "prog%d", ch);
    return progs_get(PROGS_NS_SYS, k);
}

static esp_err_t erase_key(nvs_handle_t h, const char *k)
{
    esp_err_t r = nvs_erase_key(h, k);
    return r == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : r;
}

static esp_err_t set_or_erase(nvs_handle_t h, const char *k, const char *s)
{
    return (s && s[0]) ? nvs_set_str(h, k, s) : erase_key(h, k);
}

esp_err_t settings_save(void)
{
    nvs_handle_t h;
    esp_err_t r = nvs_open(NS, NVS_READWRITE, &h);
    if (r != ESP_OK) return r;
#define TRY(x) do { if ((r = (x)) != ESP_OK) goto out; } while (0)
    TRY(nvs_set_u8(h, "nch", g_set.nch));
    for (int i = 0; i < CHANNELS_MAX; i++) {
        char kg[8], kn[8], kp[8], ka[8], kd[8], kw[8], km[8], ko[8], kt[8], kr[8], ki[8];
        snprintf(kg, sizeof kg, "gpio%d", i); snprintf(kn, sizeof kn, "npix%d", i); snprintf(kp, sizeof kp, "prog%d", i);
        snprintf(ka, sizeof ka, "gam%d", i); snprintf(kd, sizeof kd, "dith%d", i); snprintf(kw, sizeof kw, "wire%d", i); snprintf(km, sizeof km, "wmax%d", i);
        snprintf(ko, sizeof ko, "wmod%d", i); snprintf(kt, sizeof kt, "wpt%d", i); snprintf(kr, sizeof kr, "wrat%d", i);
        snprintf(ki, sizeof ki, "cint%d", i);
        if (i < g_set.nch) {
            TRY(nvs_set_i8(h, kg, (int8_t)g_set.ch[i].gpio));
            TRY(nvs_set_u16(h, kn, g_set.ch[i].npix));
            TRY(strcmp(g_set.ch[i].wire, WIRE_DEFAULT) ? nvs_set_str(h, kw, g_set.ch[i].wire) : erase_key(h, kw));
            TRY(g_set.gamma[i] != GAMMA_OFF ? nvs_set_u16(h, ka, g_set.gamma[i]) : erase_key(h, ka));
            TRY(g_set.dither[i] ? nvs_set_u8(h, kd, g_set.dither[i]) : erase_key(h, kd));
            TRY(g_set.wmax[i] != 100 ? nvs_set_u8(h, km, g_set.wmax[i]) : erase_key(h, km));
            TRY(g_set.wmode[i] ? nvs_set_u8(h, ko, g_set.wmode[i]) : erase_key(h, ko));
            TRY(g_set.wpoint[i] != WPOINT_DEFAULT ? nvs_set_u32(h, kt, g_set.wpoint[i]) : erase_key(h, kt));
            TRY(g_set.wratio[i] != 100 ? nvs_set_u16(h, kr, g_set.wratio[i]) : erase_key(h, kr));
            uint16_t ci = render_intensity_target(i);
            TRY(ci != UNIT_ONE ? nvs_set_u16(h, ki, ci) : erase_key(h, ki));
            /* A channel added since boot isn't running yet: keep whatever program is stored for it. */
            if (i < g_nchannels) TRY(progs_set(PROGS_NS_SYS, kp, g_channels[i].source));
        } else {
            TRY(erase_key(h, kg)); TRY(erase_key(h, kn)); TRY(progs_set(PROGS_NS_SYS, kp, NULL)); TRY(erase_key(h, ka)); TRY(erase_key(h, kd)); TRY(erase_key(h, kw)); TRY(erase_key(h, km));
            TRY(erase_key(h, ko)); TRY(erase_key(h, kt)); TRY(erase_key(h, kr)); TRY(erase_key(h, ki));
        }
    }
    TRY(nvs_set_u16(h, "intens", render_intensity_target(INTENSITY_GLOBAL)));     /* mid-fade: where it is going */
    TRY(g_set.refresh > 1 ? nvs_set_u8(h, "refresh", g_set.refresh) : erase_key(h, "refresh"));
    TRY(g_set.out_parallel ? nvs_set_u8(h, "outpar", 1) : erase_key(h, "outpar"));
    TRY(progs_set(PROGS_NS_SYS, "auto", g_set.autoexec));
    TRY(progs_set(PROGS_NS_SYS, "automat", g_set.automation));
    TRY(g_set.auto_every != AUTO_EVERY_DEFAULT ? nvs_set_u16(h, "autoevery", g_set.auto_every) : erase_key(h, "autoevery"));
    io_cfg_t dio[IO_MAX];
    default_io(dio);
    TRY(memcmp(g_set.io, dio, sizeof dio) ? nvs_set_blob(h, "io", g_set.io, sizeof g_set.io) : erase_key(h, "io"));
    TRY(set_or_erase(h, "ssid", g_set.ssid));
    TRY(set_or_erase(h, "pass", g_set.pass));
    TRY(set_or_erase(h, "ntp", g_set.ntp));
    TRY(set_or_erase(h, "tz", g_set.tz));
    if (g_set.has_loc) { TRY(nvs_set_i32(h, "lat", g_set.lat)); TRY(nvs_set_i32(h, "lon", g_set.lon)); }
    else { nvs_erase_key(h, "lat"); nvs_erase_key(h, "lon"); }
    TRY(set_or_erase(h, "token", g_set.token));
    TRY(set_or_erase(h, "host", g_set.hostname));
    TRY(nvs_commit(h));
#undef TRY
out:
    nvs_close(h);
    return r;
}

static void dump_str(cmd_out_t *o, nvs_handle_t h, const char *label, const char *key)
{
    char *s = get_str(h, key);
    out_printf(o, "  %-10s %s\n", label, s ? s : "(not stored)");
    free(s);
}

void settings_dump(cmd_out_t *o)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) { out_puts(o, "nothing stored (defaults in use)"); return; }
    out_puts(o, "stored configuration:");
    uint8_t nch = 0;
    if (nvs_get_u8(h, "nch", &nch) == ESP_OK) out_printf(o, "  %-10s %u\n", "channels", nch);
    else out_printf(o, "  %-10s (not stored)\n", "channels");
    for (int i = 0; i < nch && i < CHANNELS_MAX; i++) {
        char k[8], label[12], gs[8] = "off"; int8_t gpio = -1; uint16_t npix = 0, gam = GAMMA_OFF; uint8_t dith = 0, wm = 100;
        snprintf(k, sizeof k, "gpio%d", i); nvs_get_i8(h, k, &gpio);
        snprintf(k, sizeof k, "npix%d", i); nvs_get_u16(h, k, &npix);
        snprintf(k, sizeof k, "gam%d", i);
        if (nvs_get_u16(h, k, &gam) == ESP_OK) gamma_format(gs, gam);
        snprintf(k, sizeof k, "dith%d", i); nvs_get_u8(h, k, &dith);
        snprintf(k, sizeof k, "wmax%d", i); nvs_get_u8(h, k, &wm);
        snprintf(k, sizeof k, "wire%d", i);
        char *ws = get_str(h, k);
        uint8_t wmo = 0; uint32_t wp = WPOINT_DEFAULT; uint16_t wr = 100;
        snprintf(k, sizeof k, "wmod%d", i); nvs_get_u8(h, k, &wmo);
        snprintf(k, sizeof k, "wpt%d", i); nvs_get_u32(h, k, &wp);
        snprintf(k, sizeof k, "wrat%d", i); nvs_get_u16(h, k, &wr);
        out_printf(o, "  ch%-8d gpio %d, %u px, wire %s, gamma %s, dither %u, white %s #%06lX x%u.%02u max %u%%\n", i, gpio, npix,
                   ws ? ws : WIRE_DEFAULT, gs, dith, settings_wmode_name(wmo), (unsigned long)wp, wr / 100, wr % 100, wm);
        free(ws);
        snprintf(k, sizeof k, "prog%d", i); snprintf(label, sizeof label, "prog%d", i);
        char *ps = progs_get(PROGS_NS_SYS, k);
        out_printf(o, "  %-10s %s\n", label, ps ? ps : "(not stored)");
        free(ps);
    }
    char *as = progs_get(PROGS_NS_SYS, "auto");
    out_printf(o, "  %-10s %s\n", "autoexec", as ? as : "(not stored)");
    free(as);
    as = progs_get(PROGS_NS_SYS, "automat");
    uint16_t ev = AUTO_EVERY_DEFAULT;
    nvs_get_u16(h, "autoevery", &ev);
    out_printf(o, "  %-10s every %u s: %s\n", "automation", ev, as ? as : "(not stored)");
    free(as);
    uint16_t v;
    if (nvs_get_u16(h, "intens", &v) == ESP_OK) out_printf(o, "  %-10s %u/256\n", "intensity", v);
    uint8_t rf = 1, par = 0;
    nvs_get_u8(h, "refresh", &rf); nvs_get_u8(h, "outpar", &par);
    out_printf(o, "  %-10s refresh x%u, parallel %s\n", "output", rf, par ? "on" : "off");
    io_cfg_t io[IO_MAX];
    size_t iol = sizeof io;
    esp_err_t ioe = nvs_get_blob(h, "io", io, &iol);
    if (ioe == ESP_OK && iol == sizeof io) {
        for (int i = 0; i < IO_MAX; i++) {
            if (io[i].gpio < 0) continue;
            io[i].name[IO_NAME_MAX] = 0;
            out_printf(o, "  io%-8d gpio %d, %s", i, io[i].gpio, io_mode_name(io[i].mode));
            if (io[i].mode == IO_PWM) out_printf(o, " %u Hz", io[i].freq);
            if (io[i].mode == IO_INPUT) out_printf(o, ", pull %s, debounce %u ms%s", io_pull_name(io[i].pull), io[i].debounce, io[i].trigger ? ", trigger" : "");
            out_printf(o, "%s%s%s\n", io[i].invert ? ", inverted" : "", io[i].name[0] ? ", name " : "", io[i].name);
        }
    } else if (ioe == ESP_ERR_NVS_INVALID_LENGTH || (ioe == ESP_OK && iol == IO_MAX * IO_CFG_V1_SIZE))
        out_printf(o, "  %-10s stored by an older version (outputs only; save to update)\n", "io");
    else out_printf(o, "  %-10s (not stored: defaults)\n", "io");
    dump_str(o, h, "wifi ssid", "ssid");
    static const char *const secret[][2] = { { "wifi pass", "pass" }, { "token", "token" } };
    for (int i = 0; i < 2; i++) {
        char *p = get_str(h, secret[i][1]);
        out_printf(o, "  %-10s %s\n", secret[i][0], p ? "******** (stored)" : "(not stored)");
        free(p);
    }
    dump_str(o, h, "ntp", "ntp");
    dump_str(o, h, "tz", "tz");
    int32_t la, lo;
    if (nvs_get_i32(h, "lat", &la) == ESP_OK && nvs_get_i32(h, "lon", &lo) == ESP_OK)
        out_printf(o, "  %-10s %ld %ld (1e-5 degrees)\n", "location", (long)la, (long)lo);
    dump_str(o, h, "hostname", "host");
    nvs_close(h);
    nvs_stats_t st;
    if (nvs_get_stats(NULL, &st) == ESP_OK)
        out_printf(o, "  NVS: %u of %u entries used\n", (unsigned)st.used_entries, (unsigned)st.total_entries);
    size_t pu, pt;
    progs_stats(&pu, &pt);
    if (progs_separate()) out_printf(o, "  programs: %u of %u entries used (progs partition)\n", (unsigned)pu, (unsigned)pt);
    else out_puts(o, "  programs: in the settings NVS (no progs partition: flash the current layout over USB)");
}

const char *settings_check_loc(double lat, double lon)
{
    if (!(lat >= -90 && lat <= 90)) return "latitude must be -90..90";
    if (!(lon >= -180 && lon <= 180)) return "longitude must be -180..180";
    return NULL;
}

void settings_set_loc(double lat, double lon)
{
    g_set.lat = (int32_t)lround(lat * 1e5);
    g_set.lon = (int32_t)lround(lon * 1e5);
    g_set.has_loc = true;
}

void settings_apply_tz(void)
{
    setenv("TZ", g_set.tz[0] ? g_set.tz : DEFAULT_TZ, 1);
    tzset();
}

const char *settings_wmode_name(uint8_t mode) { return mode == WMODE_EXTRACT ? "extract" : mode == WMODE_BOOST ? "boost" : "direct"; }
int settings_wmode_parse(const char *s)
{
    return !strcmp(s, "direct") ? WMODE_DIRECT : !strcmp(s, "extract") ? WMODE_EXTRACT : !strcmp(s, "boost") ? WMODE_BOOST : -1;
}

const char *settings_check_wire(const char *s, wire_t *w, char *norm) { return wire_parse(s, w, norm); }

const char *settings_check_gpio(int gpio)
{
    if (!GPIO_IS_VALID_OUTPUT_GPIO(gpio)) return "can't be an output on this chip";
    if (s_sys_reserved & BIT64(gpio)) return "is reserved (flash/PSRAM)";
    return NULL;
}

const char *settings_check_gpio_in(int gpio)
{
    if (!GPIO_IS_VALID_GPIO(gpio)) return "no such gpio on this chip";
    if (s_sys_reserved & BIT64(gpio)) return "is reserved (flash/PSRAM)";
    return NULL;
}

const char *settings_check_npix(long npix)
{
    return (npix < 1 || npix > NPIX_MAX) ? "npix must be 1.." NPIX_MAX_STR : NULL;
}

const char *settings_check_wifi(const char *ssid, const char *pass)
{
    if (!ssid[0]) return "ssid is empty";
    if (strlen(ssid) > 32) return "ssid is longer than 32 characters";
    size_t n = strlen(pass);
    if (n && (n < 8 || n > 64)) return "password must be 8..64 characters (or empty for an open network)";
    return NULL;
}

const char *settings_check_token(const char *token)
{
    size_t n = strlen(token);
    if (n < 8 || n >= sizeof g_set.token) return "token must be 8..64 characters";
    for (const char *p = token; *p; p++) if (*p <= ' ' || *p > '~') return "token must be printable without spaces";
    return NULL;
}

/* DNS label: 1..32 of a-z 0-9 -, not starting or ending with '-'. Lowercase only, so the name you set is the name you get. */
const char *settings_check_hostname(const char *name)
{
    size_t n = strlen(name);
    if (n < 1 || n > 32) return "hostname must be 1..32 characters";
    if (name[0] == '-' || name[n - 1] == '-') return "hostname can't start or end with '-'";
    for (const char *p = name; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-')) return "hostname may only use a-z, 0-9 and '-'";
    return NULL;
}

/* No early exit, so response time doesn't reveal how much of the token matched. */
bool settings_token_ok(const char *given)
{
    char want[sizeof g_set.token];
    cmd_lock();
    memcpy(want, g_set.token, sizeof want);
    cmd_unlock();
    size_t lw = strlen(want), lg = strlen(given);
    unsigned diff = (lw != lg) | (lw == 0);
    for (size_t i = 0; i < lw; i++) diff |= (unsigned char)want[i] ^ (unsigned char)(i < lg ? given[i] : 0);
    memset(want, 0, sizeof want);
    return diff == 0;
}

esp_err_t settings_erase(void)
{
    nvs_handle_t h;
    esp_err_t r = nvs_open(NS, NVS_READWRITE, &h);
    if (r != ESP_OK) return r;
    r = nvs_erase_all(h);
    if (r == ESP_OK) r = nvs_commit(h);
    nvs_close(h);
    if (r == ESP_OK) r = progs_erase_ns(PROGS_NS_SYS);    /* channel programs, autoexec; the library is kept */
    return r;
}
