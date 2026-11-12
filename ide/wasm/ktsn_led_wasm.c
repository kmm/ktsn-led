/* The DSL core (components/dsl) compiled to WebAssembly for the IDE: the same compiler and runtime as the boards,
 * so compile errors and the pixel preview match a real device exactly. One simulated channel plus the 8 global
 * buffers, like a board's. Strings returned to JS point at static buffers, valid until the next call. */
#define _POSIX_C_SOURCE 200809L   /* strdup */
#include <emscripten/emscripten.h>
#include <string.h>
#include "dsl_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include "dither.h"
#include "wire.h"
#include "gamma.h"
#include "astro.h"

/* Same values as main/config.h on the boards (that header needs ESP-IDF, so they are repeated here). */
#define GLOBAL_BUF_PIXELS 256
#define CHANNEL_SCRATCH   2048

static env_t     s_env = { .intensity = UNIT_ONE };
static globals_t s_globals;
static channel_t s_ch;
static bool      s_ready;
static char      s_err[DSL_ERR_LEN];
static pixel_t  *s_out;                               /* lc_output: bytes the board would send, npix long */

/* ---- automation: the preview has no board, so a stand-in host: intensities are remembered, play/stop do nothing.
 * Library names are checked against the list the IDE sets (the connected board's library), or accepted if none. ---- */
static uint16_t s_int[9] = { 256, 256, 256, 256, 256, 256, 256, 256, 256 };   /* [8] = global */
static char    *s_lib;                                 /* "\nname\nname\n", or NULL: accept any name */
static int         wh_n(void) { return 8; }
static uint16_t    wh_get(int ch) { return s_int[ch < 0 ? 8 : ch]; }
static void        wh_set(int ch, uint16_t v, uint32_t fade) { (void)fade; s_int[ch < 0 ? 8 : ch] = v; }
static const char *wh_play(int ch, const char *name) { (void)ch; (void)name; return NULL; }
static const char *wh_stop(int ch) { (void)ch; return NULL; }
static bool        wh_exists(const char *name)
{
    if (!s_lib) return true;
    char key[24];
    snprintf(key, sizeof key, "\n%s\n", name);
    return strstr(s_lib, key) != NULL;
}
/* GPIO slots: the connected board's (lc_set_io, with modes), or, without a board, every slot 0..7 exists and any
 * name is accepted: each new name gets the next slot (so the preview can set inputs by name) and modes are unknown
 * (the compiler then doesn't check in/analog/io kinds). Output values are remembered so (io ...) reads back what
 * (io-set ...) wrote; input values come from the preview's controls (lc_set_input). */
static bool     s_io_known;
static bool     s_io_used[8];
static char     s_io_name[8][16];
static int8_t   s_io_mode[8];
static uint16_t s_io_val[8];
static uint32_t s_in_val[8];                       /* digital: 0/1, analog: 0..65536 */
static char     s_off_name[8][16];                 /* offline: names in the order programs used them */
static bool     io_name_ok(const char *s)
{
    size_t n = strlen(s);
    if (!n || n > 15 || s[0] < 'a' || s[0] > 'z') return false;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-' || s[i] == '_')) return false;
    return true;
}
static int         wh_io_find(const char *name)
{
    if (!s_io_known) {
        if (!io_name_ok(name)) return -1;
        for (int i = 0; i < 8; i++) if (!strcmp(s_off_name[i], name)) return i;
        for (int i = 0; i < 8; i++) if (!s_off_name[i][0]) { snprintf(s_off_name[i], sizeof s_off_name[i], "%s", name); return i; }
        return 0;                                  /* more than 8 names: they share slot 0 */
    }
    for (int i = 0; i < 8; i++) if (s_io_used[i] && s_io_name[i][0] && !strcmp(s_io_name[i], name)) return i;
    return -1;
}
static bool        wh_io_used(int n) { return n >= 0 && n < 8 && (!s_io_known || s_io_used[n]); }
static uint16_t    wh_io_get(int n) { return s_io_val[n]; }
static const char *wh_io_set(int n, uint16_t v) { s_io_val[n] = v; return NULL; }
static int         wh_io_mode(int n) { return s_io_known && n >= 0 && n < 8 && s_io_used[n] ? s_io_mode[n] : -1; }
static bool        wh_in_get(int n) { return n >= 0 && n < 8 && s_in_val[n] != 0; }
static uint32_t    wh_analog_get(int n, int samples) { (void)samples; return n >= 0 && n < 8 ? s_in_val[n] : 0; }

/* What (use name) sees: set by the IDE (the connected board's library, or the local one). src NULL: not usable, why says so. */
typedef struct lib_src { struct lib_src *next; char *name, *src, *why; } lib_src_t;
static lib_src_t *s_srcs;
static const lib_src_t *lib_find(const char *name) { for (lib_src_t *e = s_srcs; e; e = e->next) if (!strcmp(e->name, name)) return e; return NULL; }
static char *wh_lib_load(const char *name)
{
    const lib_src_t *e = lib_find(name);
    return e && e->src ? strdup(e->src) : NULL;
}
static const char *wh_lib_why(const char *name) { const lib_src_t *e = lib_find(name); return e ? e->why : NULL; }
static const dsl_host_t k_wasm_host = { wh_n, wh_get, wh_set, wh_play, wh_stop, wh_exists, wh_io_find, wh_io_used, wh_io_get, wh_io_set,
                                        wh_lib_load, wh_lib_why, wh_io_mode, wh_in_get, wh_analog_get };

/* (Re)create the simulated channel with npix pixels. Globals are created once and keep their contents. */
EMSCRIPTEN_KEEPALIVE int lc_init(int npix)
{
    if (!s_globals.npix && globals_init(&s_globals, GLOBAL_BUF_PIXELS)) return -1;
    uint8_t ctx = s_ch.ctx;
    if (s_ready) channel_free(&s_ch);
    s_ready = channel_init(&s_ch, (uint16_t)npix, CHANNEL_SCRATCH, &s_env, &s_globals) == 0;
    s_ch.ctx = ctx;                                       /* the context outlives re-creating the channel */
    dsl_set_host(&k_wasm_host);
    free(s_out);
    s_out = s_ready ? calloc((size_t)npix, sizeof *s_out) : NULL;
    return s_ready && s_out ? 0 : -1;
}

/* 0 = render programs (default), 1 = the automation program: what check/load compile for. */
EMSCRIPTEN_KEEPALIVE void lc_set_context(int ctx) { s_ch.ctx = ctx == DSL_CTX_AUTOMATION ? DSL_CTX_AUTOMATION : DSL_CTX_RENDER; dsl_set_host(&k_wasm_host); }

/* Library names for (play ...) checks, newline separated; "-" = no list (accept any). */
EMSCRIPTEN_KEEPALIVE void lc_set_library(const char *names)
{
    free(s_lib);
    s_lib = NULL;
    if (!strcmp(names, "-")) return;
    size_t n = strlen(names);
    s_lib = malloc(n + 3);
    if (s_lib) { s_lib[0] = '\n'; memcpy(s_lib + 1, names, n); s_lib[n + 1] = '\n'; s_lib[n + 2] = 0; }
}

EMSCRIPTEN_KEEPALIVE void lc_use_clear(void)
{
    while (s_srcs) { lib_src_t *e = s_srcs; s_srcs = e->next; free(e->name); free(e->src); free(e->why); free(e); }
}
/* Add a library program for (use name). why non-empty: the name can't be used, and this is the error. */
EMSCRIPTEN_KEEPALIVE void lc_use_add(const char *name, const char *src, const char *why)
{
    lib_src_t *e = calloc(1, sizeof *e);
    if (!e) return;
    e->name = strdup(name);
    if (why[0]) e->why = strdup(why); else e->src = strdup(src);
    e->next = s_srcs;
    s_srcs = e;
}

/* The connected board's slots for (io/in/analog ...) checks: "n:name[:mode]" lines (name may be empty; mode binary,
 * pwm, input or analog); "-" = no board (accept any name). */
EMSCRIPTEN_KEEPALIVE void lc_set_io(const char *desc)
{
    static const char *const modes[] = { "binary", "pwm", "input", "analog" };
    memset(s_io_used, 0, sizeof s_io_used);
    memset(s_io_name, 0, sizeof s_io_name);
    memset(s_io_mode, -1, sizeof s_io_mode);
    s_io_known = strcmp(desc, "-") != 0;
    if (!s_io_known) return;
    for (const char *p = desc; *p; ) {
        char *end;
        long n = strtol(p, &end, 10);
        if (end == p || *end != ':') break;
        p = end + 1;
        size_t l = strcspn(p, "\n"), nl = strcspn(p, ":\n");
        if (n >= 0 && n < 8) {
            s_io_used[n] = true;
            snprintf(s_io_name[n], sizeof s_io_name[n], "%.*s", (int)(nl < 15 ? nl : 15), p);
            if (nl < l)
                for (int m = 0; m < 4; m++)
                    if (l - nl - 1 == strlen(modes[m]) && !strncmp(p + nl + 1, modes[m], l - nl - 1)) s_io_mode[n] = (int8_t)m;
        }
        p += l;
        if (*p == '\n') p++;
    }
}

/* What an input reads in the preview: slot >= 0, or by name (slot -1; offline a new name gets its slot here).
 * value: digital 0/1, analog 0..65536. */
EMSCRIPTEN_KEEPALIVE void lc_set_input(int slot, const char *name, uint32_t value)
{
    if (slot < 0) slot = wh_io_find(name);
    if (slot >= 0 && slot < 8) s_in_val[slot] = value > 65536 ? 65536 : value;
}

/* Compile only (no change to what runs). "" if fine, else "line L col C: message". */
EMSCRIPTEN_KEEPALIVE const char *lc_check(const char *src)
{
    s_err[0] = 0;
    program_t *p = dsl_compile(&s_ch, src, s_err, sizeof s_err);
    if (p) { program_free(p); s_err[0] = 0; }
    return s_err;
}

/* Compile and make it the running program (from the next lc_render). On error the old program keeps running. */
EMSCRIPTEN_KEEPALIVE const char *lc_load(const char *src)
{
    s_err[0] = 0;
    if (channel_set_source(&s_ch, src, s_err, sizeof s_err)) return s_err;
    s_ch.perf.faults = 0;
    s_ch.perf.last_err = NULL;
    return "";
}

/* Like the boards' autoexec: run once in a temporary channel whose X Y Z O are GLOBAL_BUF_PIXELS long. */
EMSCRIPTEN_KEEPALIVE const char *lc_autoexec(const char *src)
{
    channel_t tmp;
    s_err[0] = 0;
    if (channel_init(&tmp, GLOBAL_BUF_PIXELS, CHANNEL_SCRATCH, &s_env, &s_globals)) { strcpy(s_err, "out of memory"); return s_err; }
    if (channel_set_source(&tmp, src, s_err, sizeof s_err) == 0) {
        channel_render(&tmp);
        if (tmp.perf.faults) { strncpy(s_err, tmp.perf.last_err, sizeof s_err - 1); s_err[sizeof s_err - 1] = 0; }
    }
    channel_free(&tmp);
    return s_err;
}

EMSCRIPTEN_KEEPALIVE int         lc_render(void)       { return (int)channel_render(&s_ch); }
EMSCRIPTEN_KEEPALIVE int         lc_faults(void)       { return (int)s_ch.perf.faults; }
EMSCRIPTEN_KEEPALIVE const char *lc_last_error(void)   { return s_ch.perf.last_err ? s_ch.perf.last_err : ""; }
EMSCRIPTEN_KEEPALIVE int         lc_scratch_high(void) { return (int)s_ch.scratch.high; }
EMSCRIPTEN_KEEPALIVE void        lc_seed(uint32_t s)   { s_ch.rng = s ? s : 1; }

/* Buffers: 0..3 = X Y Z O of the channel, 4..11 = G0..G7. Pixels are 0xWWRRGGBB. */
EMSCRIPTEN_KEEPALIVE pixel_t *lc_buf(int id)
{
    if (id >= 0 && id < BUF_COUNT) return s_ch.buf[id];
    if (id >= BUF_COUNT && id < BUF_COUNT + DSL_NUM_GLOBALS) return s_globals.buf[id - BUF_COUNT];
    return NULL;
}
EMSCRIPTEN_KEEPALIVE int lc_buf_len(int id) { return id < BUF_COUNT ? s_ch.npix : s_globals.npix; }

/* The board's gamma table for gamma in hundredths (100 = off): 256 bytes, valid until the next call. */
EMSCRIPTEN_KEEPALIVE const uint8_t *lc_gamma_lut(int g100)
{
    static uint8_t lut[256];
    gamma_lut(lut, (unsigned)g100);
    return lut;
}

/* Buffer O through a board's output stage (dither.h, wire.h: gamma, white mode, intensity, white limit, dither for this
 * frame, W spread over the wire's W outputs), for display: 0xWWRRGGBB with R G B as sent and W as the white level
 * 0..255 = (sum of the W outputs) / (number of them); 0 if the wire has no W. npix long, valid until the next call. */
EMSCRIPTEN_KEEPALIVE const pixel_t *lc_output(int g100, int intensity16, int dither, uint32_t frame, const char *wire,
                                             int wmode, uint32_t wpoint, int wratio100, int wmax100)
{
    static uint8_t l8[256];
    static uint16_t l16[256];
    static int built = -1;
    if (!s_out) return NULL;
    if (built != g100) { gamma_lut(l8, (unsigned)g100); gamma_lut16(l16, (unsigned)g100); built = g100; }
    wire_t w; char norm[WIRE_STR_MAX];
    if (wire_parse(wire, &w, norm)) wire_parse(WIRE_DEFAULT, &w, norm);
    out_map_t m = { .intensity16 = (uint32_t)(intensity16 < 0 ? 0 : intensity16 > (int)OUT_ONE ? (int)OUT_ONE : intensity16),
                    .gamma = g100 == GAMMA_OFF ? NULL : l8, .gamma16 = l16,
                    .dither = (uint8_t)(dither < 0 ? 0 : dither > DITHER_MAX ? DITHER_MAX : dither), .frame = frame,
                    .wmax = (uint16_t)(((wmax100 < 0 ? 0 : wmax100 > 100 ? 100 : wmax100) * UNIT_ONE + 50) / 100),
                    .wmode = (uint8_t)(wmode >= WMODE_DIRECT && wmode <= WMODE_BOOST ? wmode : WMODE_DIRECT) };
    wire_white_factors(wpoint, (uint32_t)(wratio100 < 10 ? 10 : wratio100 > 400 ? 400 : wratio100), m.wf);
    uint8_t b[WIRE_MAX];
    for (uint32_t i = 0; i < s_ch.npix; i++) {
        wire_encode_mapped(&w, &m, s_ch.buf[BUF_O][i], i, b);
        uint32_t c[3] = { 0, 0, 0 }, seen = 0, wsum = 0;
        for (uint8_t k = 0; k < w.n; k++) {
            uint8_t sh = w.shift[k];
            if (sh == 24) wsum += b[k];
            else if (sh != WIRE_ZERO && !(seen & (1u << sh / 8))) { c[(16 - sh) / 8] = b[k]; seen |= 1u << sh / 8; }
        }
        uint32_t wl = w.nw ? (wsum + w.nw / 2) / w.nw : 0;
        s_out[i] = wl << 24 | c[0] << 16 | c[1] << 8 | c[2];
    }
    return s_out;
}

/* Clear every buffer (channel and globals), e.g. when the preview is reset. */
EMSCRIPTEN_KEEPALIVE void lc_clear(void)
{
    for (int i = 0; i < BUF_COUNT; i++) memset(s_ch.buf[i], 0, s_ch.npix * sizeof(pixel_t));
    for (int i = 0; i < DSL_NUM_GLOBALS; i++) memset(s_globals.buf[i], 0, s_globals.npix * sizeof(pixel_t));
}

/* Sun and moon for the preview (the same code as a board). Unix seconds; -1 = no such event in [from, to). */
EMSCRIPTEN_KEEPALIVE double lc_sun_event(double lat, double lon, double alt, int rising, double from, double to)
{
    int64_t t;
    return astro_sun_event(lat, lon, alt, rising != 0, (int64_t)from, (int64_t)to, &t) ? (double)t : -1;
}
EMSCRIPTEN_KEEPALIVE double lc_sun_altitude(double lat, double lon, double t) { return astro_sun_altitude(lat, lon, (int64_t)t); }
EMSCRIPTEN_KEEPALIVE double lc_moon_angle(double t) { return astro_moon_angle((int64_t)t); }
EMSCRIPTEN_KEEPALIVE double lc_moon_lit(double t)   { return astro_moon_lit((int64_t)t); }

EMSCRIPTEN_KEEPALIVE void lc_set_env(uint32_t frame, int year, int month, int day, int doy, int hour, int minute, int second,
                                     int intensity, int sunrise, int sunset, int dawn, int dusk, int moon, int moonlit,
                                     int ms, int weekday, int timeset, int daylight)
{
    s_env.frame = frame;
    s_env.year = (uint16_t)year; s_env.month = (uint8_t)month; s_env.day = (uint8_t)day; s_env.doy = (uint16_t)doy;
    s_env.hour = (uint8_t)hour; s_env.minute = (uint8_t)minute; s_env.second = (uint8_t)second;
    s_env.intensity = (uint16_t)intensity;
    s_env.sunrise = (int16_t)sunrise; s_env.sunset = (int16_t)sunset; s_env.dawn = (int16_t)dawn; s_env.dusk = (int16_t)dusk;
    s_env.moon = (int16_t)moon; s_env.moonlit = (int16_t)moonlit;
    s_env.ms = (uint16_t)ms; s_env.weekday = (uint8_t)weekday; s_env.timeset = (uint8_t)timeset; s_env.daylight = (int8_t)daylight;
}

/* The native table, for autocomplete and reference coverage. */
EMSCRIPTEN_KEEPALIVE int         lc_native_count(void)    { return dsl_native_count(); }
EMSCRIPTEN_KEEPALIVE const char *lc_native_name(int i)    { const native_def_t *d = dsl_native_at(i); return d ? d->name : ""; }
EMSCRIPTEN_KEEPALIVE int         lc_native_min_args(int i) { const native_def_t *d = dsl_native_at(i); return d ? d->min_args : 0; }
EMSCRIPTEN_KEEPALIVE int         lc_native_max_args(int i) { const native_def_t *d = dsl_native_at(i); return d ? d->max_args : 0; }
