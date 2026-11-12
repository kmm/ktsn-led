/* Console commands (shared by serial and network transports through cmd.c) and the serial REPL. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "app.h"
#include "cmd.h"
#include "config.h"
#include "dither.h"
#include "gamma.h"
#include "io.h"
#include "automation.h"
#include "firmware.h"
#include "progs.h"
#include "sdkconfig.h"
#if CONFIG_KTSN_WIFI
#include "net.h"
#endif
#include "led_out.h"
#include "render.h"
#include "sky.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_KTSN_SERIAL_API
#include "serial_api.h"
#endif
#include "settings.h"
#include "esp_console.h"
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "api.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#endif
#include "esp_system.h"
#include "esp_timer.h"

static channel_t *chan_arg(cmd_out_t *o, const char *s)
{
    char *end;
    long i = strtol(s, &end, 10);
    if (*s == 0 || *end || i < 0 || i >= g_nchannels) { out_printf(o, "bad channel '%s' (0..%d)\n", s, g_nchannels - 1); return NULL; }
    return &g_channels[i];
}

/* The line was split on whitespace, so re-join argv[from..] into one source string. */
static bool join_args(cmd_out_t *o, int argc, char **argv, int from, char *src, size_t cap)
{
    if (argc >= CMD_MAX_ARGS - 1) { out_puts(o, "too many words on one line"); return false; }   /* split_argv truncated */
    size_t n = 0;
    for (int i = from; i < argc; i++) {
        size_t l = strlen(argv[i]);
        if (n + l + 2 > cap) { out_puts(o, "source too long for one line"); return false; }
        memcpy(src + n, argv[i], l); n += l; src[n++] = ' ';
    }
    src[n] = 0;
    return true;
}

/* Program source on the command line: the rest of the line as typed, after `from` words, so strings keep their
 * quotes and spacing. Falls back to re-joining argv when the transport has no raw line. */
static bool source_arg(cmd_out_t *o, int argc, char **argv, int from, char *src, size_t cap)
{
    if (!o->line) return join_args(o, argc, argv, from, src, cap);
    const char *p = o->line;
    for (int i = 0; i < from; i++) { while (*p == ' ') p++; while (*p && *p != ' ') p++; }
    while (*p == ' ') p++;
    size_t n = strlen(p);
    while (n && isspace((unsigned char)p[n - 1])) n--;
    if (n + 1 > cap) { out_puts(o, "source too long for one line"); return false; }
    memcpy(src, p, n);
    src[n] = 0;
    return true;
}

/* Multi-line entry: lines until one that is only ".", joined with newlines (comments and indentation kept).
 * Returns the malloc'd source, or NULL (cancelled, too long, or a transport nobody types into). The command lock is
 * released while waiting, so someone typing slowly doesn't hold up the network API; callers must not rely on state
 * read before the call. After an overlong line or program the rest is still read up to the "." and then dropped, so
 * the remaining lines of a paste never run as commands. */
static char *read_source(cmd_out_t *o, const char *what)
{
    if (!o->read_line) { out_puts(o, "multi-line entry needs the serial console or telnet"); return NULL; }
    char *src = malloc(SOURCE_MAX + 1), *line = malloc(CMD_LINE_MAX);
    if (!src || !line) { free(src); free(line); out_puts(o, "out of memory"); return NULL; }
    out_printf(o, "enter %s, then a line with only '.' to finish (Ctrl-C cancels):\n", what);
    size_t n = 0;
    enum { OK, LONG_LINE, LONG_PROG, CANCELLED, CLOSED } st = OK;
    cmd_unlock();
    for (;;) {
        o->write(o, "... ", 4);
        int r = o->read_line(o, line, CMD_LINE_MAX);
        if (r == CMD_READ_CANCEL) { st = CANCELLED; break; }
        if (r == CMD_READ_CLOSED) { st = CLOSED; break; }
        if (r == CMD_READ_TOO_LONG) { if (st == OK) st = LONG_LINE; continue; }
        if (!strcmp(line, ".")) break;
        if (n + (size_t)r + 1 > SOURCE_MAX) { if (st == OK) st = LONG_PROG; continue; }
        memcpy(src + n, line, (size_t)r);
        n += (size_t)r;
        src[n++] = '\n';
    }
    cmd_lock();
    free(line);
    if (st == LONG_LINE) out_printf(o, "a line was too long (max %d characters per line), nothing changed\n", CMD_LINE_MAX - 1);
    if (st == LONG_PROG) out_printf(o, "program too long (max %d characters), nothing changed\n", SOURCE_MAX);
    if (st == CANCELLED) out_puts(o, "cancelled, nothing changed");
    if (st != OK) { free(src); return NULL; }
    src[n] = 0;
    return src;
}

/* prog <ch> <source...>, or prog <ch> alone for multi-line entry */
static int cmd_prog(cmd_out_t *o, int argc, char **argv)
{
    if (argc < 2) { out_puts(o, "usage: prog <ch> (<dsl source>)   or: prog <ch>  to enter it on several lines"); return 1; }
    channel_t *ch = chan_arg(o, argv[1]);
    if (!ch) return 1;
    char line_src[CMD_LINE_MAX], *src = line_src, err[DSL_ERR_LEN];
    if (argc == 2) {
        char label[24];
        snprintf(label, sizeof label, "the program for ch%s", argv[1]);
        if (!(src = read_source(o, label))) return 1;
    } else if (!source_arg(o, argc, argv, 2, line_src, sizeof line_src)) return 1;
    int rc = channel_set_source(ch, src, err, sizeof err);
    if (src != line_src) free(src);
    if (rc) { out_printf(o, "error: %s\n", err); return 1; }
    out_puts(o, "ok (takes effect next frame)");
    return 0;
}

static int cmd_list(cmd_out_t *o, int argc, char **argv)
{
    for (int i = 0; i < g_nchannels; i++) {
        if (argc > 1 && g_channels + i != chan_arg(o, argv[1])) continue;
        out_printf(o, "ch%d: %s\n", i, g_channels[i].source ? g_channels[i].source : "(none)");
    }
    if (argc == 1) out_printf(o, "autoexec: %s\n", g_set.autoexec ? g_set.autoexec : "(none)");
    return 0;
}

/* env time YYYY-MM-DD HH:MM:SS, for when there is no NTP. */
static int set_time(cmd_out_t *o, const char *date, const char *clock)
{
    struct tm tm = {0};
    if (sscanf(date, "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3 ||
        sscanf(clock, "%d:%d:%d", &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 3) {
        out_puts(o, "usage: env time YYYY-MM-DD HH:MM:SS"); return 1;
    }
    struct tm want = tm;
    tm.tm_year -= 1900; tm.tm_mon -= 1; tm.tm_isdst = -1;
    time_t t = mktime(&tm);       /* normalizes out-of-range fields, so compare to catch e.g. Feb 30 */
    if (t == (time_t)-1 || tm.tm_year + 1900 != want.tm_year || tm.tm_mon + 1 != want.tm_mon || tm.tm_mday != want.tm_mday ||
        tm.tm_hour != want.tm_hour || tm.tm_min != want.tm_min || tm.tm_sec != want.tm_sec) {
        out_puts(o, "invalid date or time"); return 1;
    }
    struct timeval tv = { .tv_sec = t };
    settimeofday(&tv, NULL);
    out_printf(o, "time set to %04d-%02d-%02d %02d:%02d:%02d\n", want.tm_year, want.tm_mon, want.tm_mday,
               want.tm_hour, want.tm_min, want.tm_sec);
    return 0;
}

static int cmd_env(cmd_out_t *o, int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "intensity")) {
        int v = atoi(argv[2]);
        render_set_intensity(INTENSITY_GLOBAL, (uint16_t)(v < 0 ? 0 : v > 256 ? 256 : v), 0);
    } else if (argc >= 2 && !strcmp(argv[1], "time")) {
        if (argc == 4) return set_time(o, argv[2], argv[3]);
        out_puts(o, "usage: env time YYYY-MM-DD HH:MM:SS");
        return 1;
    }
    out_printf(o, "frame=%lu date=%04u-%02u-%02u doy=%u time=%02u:%02u:%02u intensity=%u/256\n", (unsigned long)g_env.frame,
               g_env.year, g_env.month, g_env.day, g_env.doy, g_env.hour, g_env.minute, g_env.second, g_env.intensity);
    out_printf(o, "ms=%u weekday=%u daymin=%u daysec=%lu timeset=%u\n", g_env.ms, g_env.weekday, g_env.hour * 60u + g_env.minute,
               (unsigned long)(g_env.hour * 3600u + g_env.minute * 60u + g_env.second), g_env.timeset);
    out_printf(o, "sunrise=%d sunset=%d dawn=%d dusk=%d daylight=%d moon=%d moonlit=%d\n", g_env.sunrise, g_env.sunset, g_env.dawn,
               g_env.dusk, g_env.daylight, g_env.moon, g_env.moonlit);
    return 0;
}

static int cmd_buf(cmd_out_t *o, int argc, char **argv)
{
    if (argc < 3) { out_puts(o, "usage: buf <ch> <X|Y|Z|O|G0..G7> [start] [count]"); return 1; }
    channel_t *ch = chan_arg(o, argv[1]);
    bufref_t b;
    if (!ch) return 1;
    if (!dsl_resolve_buf(ch, argv[2], &b)) { out_puts(o, "unknown buffer"); return 1; }
    int s = argc > 3 ? atoi(argv[3]) : 0, c = argc > 4 ? atoi(argv[4]) : 8;
    if (s < 0) s = 0;
    for (int i = s; i < s + c && i < b.len; i++) out_printf(o, "[%d] %08lX\n", i, (unsigned long)*buf_at(&b, i));
    return 0;
}

static void print_stat(cmd_out_t *o, const char *name, const stat_us_t *s)
{
    out_printf(o, "  %-7s last %5lu  avg %5lu  min %5lu  max %5lu us\n", name, (unsigned long)s->last,
               (unsigned long)s->avg, (unsigned long)s->min, (unsigned long)s->max);
}

static int cmd_stat(cmd_out_t *o, int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "reset")) { perf_reset(); out_puts(o, "stats reset"); return 0; }
    int64_t el = esp_timer_get_time() - g_perf.t0_us;
    uint32_t fps100 = el > 0 ? (uint32_t)((int64_t)g_perf.frames * 100000000LL / el) : 0;
    uint32_t sps100 = el > 0 ? (uint32_t)((int64_t)g_perf.sends * 100000000LL / el) : 0;
    uint32_t budget = 1000000 / (FRAME_HZ * g_set.refresh);
    out_printf(o, "fps %lu.%02lu  frames %lu  output %lu.%02lu Hz (x%u)  missed ticks %lu  tick %lu us\n",
               (unsigned long)(fps100 / 100), (unsigned long)(fps100 % 100), (unsigned long)g_perf.frames,
               (unsigned long)(sps100 / 100), (unsigned long)(sps100 % 100), g_set.refresh, (unsigned long)g_perf.missed,
               (unsigned long)budget);
    print_stat(o, "frame", &g_perf.frame);               /* render ticks: render + send */
    if (led_out_concurrent()) print_stat(o, "send", &g_perf.flush);   /* all strings at once; per-channel "output" is encoding/starting */
    out_printf(o, "  load    %lu%% of frame budget (avg)\n", (unsigned long)(g_perf.frame.avg * 100 / budget));
    out_printf(o, "render stack: %lu of %u B never used\n", (unsigned long)render_stack_free(), (unsigned)RENDER_STACK);
    for (int i = 0; i < g_nchannels; i++) {
        const channel_t *ch = &g_channels[i];
        out_printf(o, "ch%d (%u px): nodes %lu  stack est %u B  scratch hw %u/%u B  faults %lu%s%s\n", i, ch->npix,
                   (unsigned long)ch->perf.nodes_last, ch->perf.stack, (unsigned)ch->scratch.high, (unsigned)ch->scratch.size,
                   (unsigned long)ch->perf.faults, ch->perf.last_err ? "  last: " : "", ch->perf.last_err ? ch->perf.last_err : "");
        print_stat(o, "render", &ch->perf.render);
        print_stat(o, "output", &ch->perf.output);
    }
    return 0;
}

/* autoexec [run | clear | paste | <source...>]   (paste: multi-line entry) */
static int cmd_autoexec(cmd_out_t *o, int argc, char **argv)
{
    char err[DSL_ERR_LEN];
    if (argc == 1) { out_printf(o, "autoexec: %s\n", g_set.autoexec ? g_set.autoexec : "(none)"); return 0; }
    if (argc == 2 && !strcmp(argv[1], "clear")) {
        free(g_set.autoexec); g_set.autoexec = NULL;
        out_puts(o, "autoexec cleared (save to persist)");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "run")) {
        if (!g_set.autoexec) { out_puts(o, "no autoexec program"); return 1; }
        if (autoexec_run(g_set.autoexec, err, sizeof err)) { out_printf(o, "error: %s\n", err); return 1; }
        out_puts(o, "ok");
        return 0;
    }
    char line_src[CMD_LINE_MAX], *src = line_src;
    if (argc == 2 && !strcmp(argv[1], "paste")) { if (!(src = read_source(o, "the autoexec program"))) return 1; }
    else if (!source_arg(o, argc, argv, 1, line_src, sizeof line_src)) return 1;
    int rc = autoexec_run(src, err, sizeof err);              /* only keep what works */
    char *copy = rc ? NULL : src != line_src ? src : strdup(src);
    if (rc && src != line_src) free(src);
    if (rc) { out_printf(o, "error: %s\n", err); return 1; }
    if (!copy) { out_puts(o, "out of memory"); return 1; }
    free(g_set.autoexec);
    g_set.autoexec = copy;
    out_puts(o, "ok, ran once (save to run it at every boot)");
    return 0;
}

static bool gpio_usable(cmd_out_t *o, int gpio)
{
    const char *why = settings_check_gpio(gpio);
    if (why) out_printf(o, "gpio %d %s\n", gpio, why);
    return !why;
}

/* chan | chan <n> <gpio> <npix> | chan <n> off */
static int cmd_chan(cmd_out_t *o, int argc, char **argv)
{
    if (argc == 1) {
        out_printf(o, "configured (applied at boot): %u channel(s)\n", g_set.nch);
        for (int i = 0; i < g_set.nch; i++)
            out_printf(o, "  ch%d: gpio %d, %u px, wire %s%s\n", i, g_set.ch[i].gpio, g_set.ch[i].npix, g_set.ch[i].wire,
                       i >= g_nchannels ? "  (not running: save + reboot)" :
                       (g_channels[i].npix != g_set.ch[i].npix || strcmp(led_out_wire(i), g_set.ch[i].wire)
                        ? "  (changed: save + reboot)" : ""));
        if (g_nchannels > g_set.nch) out_printf(o, "  %d more running until save + reboot\n", g_nchannels - g_set.nch);
        return 0;
    }
    char *end;
    long n = strtol(argv[1], &end, 10);
    if (*end || n < 0 || n >= CHANNELS_MAX || n > g_set.nch) {
        out_printf(o, "channel must be 0..%d\n", g_set.nch < CHANNELS_MAX ? g_set.nch : CHANNELS_MAX - 1);
        return 1;
    }
    if (argc == 3 && !strcmp(argv[2], "off")) {
        if (n != g_set.nch - 1 || g_set.nch == 1) { out_puts(o, "only the last channel can be removed, and one must remain"); return 1; }
        g_set.nch--;
        out_puts(o, "ok (save + reboot to apply)");
        return 0;
    }
    if (argc < 4) { out_puts(o, "usage: chan <n> <gpio> <npix> [wire] | chan <n> off   (wire: RGB, GRB, GRBW, \"BRG WWW\"...)"); return 1; }
    long gpio = strtol(argv[2], &end, 10);
    if (*end || !gpio_usable(o, (int)gpio)) return 1;
    long npix = strtol(argv[3], &end, 10);
    const char *why = *end ? "npix must be a number" : settings_check_npix(npix);
    if (why) { out_puts(o, why); return 1; }
    char wire[WIRE_STR_MAX];                              /* given (several words allowed), kept, or the default */
    snprintf(wire, sizeof wire, "%s", n < g_set.nch ? g_set.ch[n].wire : WIRE_DEFAULT);
    if (argc > 4) {
        char joined[CMD_LINE_MAX];
        wire_t w;
        if (!join_args(o, argc, argv, 4, joined, sizeof joined)) return 1;
        if ((why = settings_check_wire(joined, &w, wire))) { out_puts(o, why); return 1; }
    }
    for (int i = 0; i < g_set.nch; i++)
        if (i != n && g_set.ch[i].gpio == gpio) { out_printf(o, "gpio %ld is already used by ch%d\n", gpio, i); return 1; }
    if (io_on_gpio((int)gpio) >= 0) { out_printf(o, "gpio %ld is output %d (io config %d off first)\n", gpio, io_on_gpio((int)gpio), io_on_gpio((int)gpio)); return 1; }
    g_set.ch[n].gpio = (int)gpio;
    g_set.ch[n].npix = (uint16_t)npix;
    snprintf(g_set.ch[n].wire, sizeof g_set.ch[n].wire, "%s", wire);
    if (n == g_set.nch) g_set.nch++;
    out_puts(o, "ok (save + reboot to apply)");
    return 0;
}

/* gamma | gamma <ch> <1.0..3.0 | off>: per-channel gamma correction at output, applies at once */
static int cmd_gamma(cmd_out_t *o, int argc, char **argv)
{
    char b[8];
    if (argc == 1) {
        for (int i = 0; i < g_set.nch; i++) { gamma_format(b, g_set.gamma[i]); out_printf(o, "  ch%d: gamma %s\n", i, b); }
        return 0;
    }
    if (argc != 3) { out_puts(o, "usage: gamma <ch> <1.0..3.0 | off>   e.g. gamma 0 2.2"); return 1; }
    char *end;
    long n = strtol(argv[1], &end, 10);
    if (!argv[1][0] || *end || n < 0 || n >= g_set.nch) { out_printf(o, "channel must be 0..%d\n", g_set.nch - 1); return 1; }
    unsigned g;
    const char *why = gamma_parse(argv[2], &g);
    if (why) { out_puts(o, why); return 1; }
    g_set.gamma[n] = (uint16_t)g;
    render_output_update((int)n);
    out_puts(o, "ok (save to persist)");
    return 0;
}

/* dither | dither <ch> <0..4>: temporal dither bits below the 8-bit output (0 = off), applies at once */
static int cmd_dither(cmd_out_t *o, int argc, char **argv)
{
    if (argc == 1) {
        for (int i = 0; i < g_set.nch; i++) out_printf(o, "  ch%d: dither %u%s\n", i, g_set.dither[i], g_set.dither[i] ? " bits" : " (off)");
        return 0;
    }
    if (argc != 3) { out_puts(o, "usage: dither <ch> <0..4>   e.g. dither 0 3  (0 = off)"); return 1; }
    char *end;
    long n = strtol(argv[1], &end, 10);
    if (!argv[1][0] || *end || n < 0 || n >= g_set.nch) { out_printf(o, "channel must be 0..%d\n", g_set.nch - 1); return 1; }
    long d = strtol(argv[2], &end, 10);
    if (!argv[2][0] || *end || d < 0 || d > DITHER_MAX) { out_printf(o, "dither must be 0..%d bits (0 = off)\n", DITHER_MAX); return 1; }
    g_set.dither[n] = (uint8_t)d;
    render_output_update((int)n);
    out_puts(o, "ok (save to persist)");
    return 0;
}

static void lib_line(const char *key, size_t len, void *ctx) { out_printf((cmd_out_t *)ctx, "  %-15s %5u chars\n", key, (unsigned)len); }

/* lib | lib show <name> | lib set <name> [source] | lib save <name> <ch> | lib run <name> <ch> | lib rm <name>
 * The program library: programs stored by name (progs partition), to put on channels by hand or from automation. */
static int cmd_lib(cmd_out_t *o, int argc, char **argv)
{
    char err[DSL_ERR_LEN];
    if (argc == 1) {
        progs_list(PROGS_NS_LIB, lib_line, o);
        size_t used, total;
        progs_stats(&used, &total);
        out_printf(o, "program storage: %u of %u entries used%s\n", (unsigned)used, (unsigned)total,
                   progs_separate() ? "" : " (shared with settings: flash the current layout over USB for more room)");
        return 0;
    }
    const char *usage = "usage: lib | lib show <name> | lib set <name> [source] | lib save <name> <ch> | lib run <name> <ch> | lib rm <name>";
    if (argc < 3) { out_puts(o, usage); return 1; }
    const char *op = argv[1], *name = argv[2];
    const char *why = lib_check_name(name);
    if (why) { out_puts(o, why); return 1; }
    if (!strcmp(op, "show") && argc == 3) {
        char *src = progs_get(PROGS_NS_LIB, name);
        if (!src) { out_printf(o, "no library program '%s'\n", name); return 1; }
        out_printf(o, "%s\n", src);
        free(src);
        return 0;
    }
    if (!strcmp(op, "rm") && argc == 3) {
        esp_err_t r = lib_del(name, err, sizeof err);
        if (r == ESP_ERR_NOT_FOUND) { out_printf(o, "no library program '%s'\n", name); return 1; }
        if (r == ESP_ERR_INVALID_STATE) { out_printf(o, "error: %s\n", err); return 1; }
        if (r != ESP_OK) { out_printf(o, "failed: %s\n", esp_err_to_name(r)); return 1; }
        out_puts(o, "removed");
        return 0;
    }
    if (!strcmp(op, "set")) {
        char line_src[CMD_LINE_MAX], *src = line_src;
        if (argc == 3) { char label[40]; snprintf(label, sizeof label, "library program '%s'", name); if (!(src = read_source(o, label))) return 1; }
        else if (!source_arg(o, argc, argv, 3, line_src, sizeof line_src)) return 1;
        why = lib_put(name, src, err, sizeof err);
        if (src != line_src) free(src);
        if (why) { out_printf(o, "error: %s\n", why); return 1; }
        out_puts(o, "stored");
        return 0;
    }
    if ((!strcmp(op, "save") || !strcmp(op, "run")) && argc == 4) {
        channel_t *ch = chan_arg(o, argv[3]);
        if (!ch) return 1;
        if (!strcmp(op, "save")) {
            if (!ch->source || !ch->source[0]) { out_puts(o, "that channel has no program"); return 1; }
            why = lib_put(name, ch->source, err, sizeof err);
            if (why) { out_printf(o, "error: %s\n", why); return 1; }
            out_puts(o, "stored");
            return 0;
        }
        char *src = progs_get(PROGS_NS_LIB, name);
        if (!src) { out_printf(o, "no library program '%s'\n", name); return 1; }
        int rc = channel_set_source(ch, src, err, sizeof err);
        free(src);
        if (rc) { out_printf(o, "error: %s\n", err); return 1; }
        out_puts(o, "ok (takes effect next frame; save keeps it on the channel)");
        return 0;
    }
    out_puts(o, usage);
    return 1;
}

/* version: what runs, from which slot, whether it is confirmed yet, and what is in the other slot (update testing). */
static int cmd_version(cmd_out_t *o, int argc, char **argv)
{
    (void)argc; (void)argv;
    fw_info_t f;
    fw_info(&f);
    out_printf(o, "KTSN LED %s (built %s %s), ESP-IDF %s, %s, %s output\n", f.version, f.date, f.time, f.idf, CONFIG_IDF_TARGET, led_out_driver());
    if (!f.has_other) { out_printf(o, "running from %s (no second app slot: no network updates)\n", f.slot); return 0; }
    out_printf(o, "running from %s: %s\n", f.slot,
               !strcmp(f.state, "pending") ? "NEW, not confirmed yet (kept once it renders for 30 s; a reset before then goes back)" :
               !strcmp(f.state, "valid") ? "confirmed" : f.state[0] ? f.state : "flashed over USB (not a network update)");
    if (f.other_valid) out_printf(o, "other slot %s: KTSN LED %s (built %s %s)\n", f.other_slot, f.other_version, f.other_date, f.other_time);
    else out_printf(o, "other slot %s: empty\n", f.other_slot);
    if (f.next_boot) out_printf(o, "next boot: %s\n", f.next_boot);
    return 0;
}

/* automation | automation <source...> | automation paste | automation clear | automation run | automation every <s>
 * The automation program (automation.h): runs every N seconds with G0..G7, env and play / stop / set-intensity. */
static int cmd_automation(cmd_out_t *o, int argc, char **argv)
{
    char err[DSL_ERR_LEN];
    if (argc == 1) {
        automation_status_t st;
        automation_get_status(&st);
        out_printf(o, "automation (every %u s): %s\n", g_set.auto_every, g_set.automation ? g_set.automation : "(none)");
        if (st.last_run < 0) out_puts(o, "  not run yet");
        else if (st.last_run_is_time) {
            char tb[24]; struct tm tm; time_t t = (time_t)st.last_run;
            localtime_r(&t, &tm);
            strftime(tb, sizeof tb, "%Y-%m-%d %H:%M:%S", &tm);
            out_printf(o, "  runs %lu, last %s\n", (unsigned long)st.runs, tb);
        } else out_printf(o, "  runs %lu, last at uptime %llds (clock not set)\n", (unsigned long)st.runs, (long long)st.last_run);
        if (st.faults) out_printf(o, "  faults %lu, last: %s\n", (unsigned long)st.faults, st.last_err ? st.last_err : "?");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "clear")) { automation_set(NULL, err, sizeof err); out_puts(o, "automation cleared (save to persist)"); return 0; }
    if (argc == 2 && !strcmp(argv[1], "run")) {
        if (!g_set.automation) { out_puts(o, "no automation program"); return 1; }
        automation_run_now();
        out_puts(o, "ok, running now");
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "every")) {
        char *end;
        long s = strtol(argv[2], &end, 10);
        if (!argv[2][0] || *end || s < 1 || s > AUTO_EVERY_MAX) { out_puts(o, "every is 1..3600 seconds"); return 1; }
        g_set.auto_every = (uint16_t)s;
        out_puts(o, "ok (save to persist)");
        return 0;
    }
    char line_src[CMD_LINE_MAX], *src = line_src;
    if (argc == 2 && !strcmp(argv[1], "paste")) { if (!(src = read_source(o, "the automation program"))) return 1; }
    else if (!source_arg(o, argc, argv, 1, line_src, sizeof line_src)) return 1;
    const char *why = automation_set(src, err, sizeof err);
    if (src != line_src) free(src);
    if (why) { out_printf(o, "error: %s\n", why); return 1; }
    out_puts(o, "ok, runs now and every few seconds (save to keep it)");
    return 0;
}

/* "0.5" / "1" / ".25" -> 0..256; -1 if not a number 0..1 (console only: floats are fine outside rendering) */
static int parse_unit(const char *s)
{
    char *end;
    double v = strtod(s, &end);
    if (!*s || *end || v < 0 || v > 1) return -1;
    return (int)(v * UNIT_ONE + 0.5);
}

/* intensity | intensity <ch|all> <0..1> [fade seconds]: per channel (and global, `all`) intensity, multiplied at
 * output; with a fade time the change happens smoothly over that many seconds. `save` keeps the targets. */
static int cmd_intensity(cmd_out_t *o, int argc, char **argv)
{
    if (argc == 1) {
        for (int i = -1; i < g_nchannels; i++) {             /* -1 = INTENSITY_GLOBAL: shown as "all" */
            uint16_t c = render_intensity(i), t = render_intensity_target(i);
            unsigned cp = (c * 100u + 128u) / 256u, tp = (t * 100u + 128u) / 256u;     /* hundredths, no float printf */
            if (i < 0) out_printf(o, "  all: %u.%02u", cp / 100, cp % 100); else out_printf(o, "  ch%d: %u.%02u", i, cp / 100, cp % 100);
            if (c != t) out_printf(o, " (fading to %u.%02u)", tp / 100, tp % 100);
            out_puts(o, "");
        }
        return 0;
    }
    if (argc != 3 && argc != 4) { out_puts(o, "usage: intensity | intensity <ch|all> <0..1> [fade seconds]"); return 1; }
    int ch = INTENSITY_GLOBAL;
    if (strcmp(argv[1], "all")) {
        char *end;
        long n = strtol(argv[1], &end, 10);
        if (!argv[1][0] || *end || n < 0 || n >= g_nchannels) { out_printf(o, "channel must be 0..%d or all\n", g_nchannels - 1); return 1; }
        ch = (int)n;
    }
    int v = parse_unit(argv[2]);
    if (v < 0) { out_puts(o, "intensity is 0..1, e.g. 0.5"); return 1; }
    uint32_t fade = 0;
    if (argc == 4) {
        char *end;
        double s = strtod(argv[3], &end);
        if (!argv[3][0] || *end || s < 0 || s > FADE_MAX_MS / 1000) { out_puts(o, "fade is 0..3600 seconds"); return 1; }
        fade = (uint32_t)(s * 1000 + 0.5);
    }
    render_set_intensity(ch, (uint16_t)v, fade);
    out_puts(o, fade ? "ok, fading (save to keep the new value)" : "ok (save to persist)");
    return 0;
}

/* white | white <ch> max <0..100> | white <ch> mode <direct|extract|boost> | white <ch> point <#RRGGBB> [ratio]
 * White handling per channel, applies at once. Several W outputs per pixel ("BRG WWW") are driven together with finer
 * steps; max limits how bright W gets; mode extract/boost make W from the white in RGB, using the white point (what
 * full W looks like in RGB) and ratio (how bright full W is compared to that RGB color, 0.1..4). */
static int cmd_white(cmd_out_t *o, int argc, char **argv)
{
    if (argc == 1) {
        for (int i = 0; i < g_set.nch; i++) {
            wire_t w; char norm[WIRE_STR_MAX];
            uint8_t nw = settings_check_wire(g_set.ch[i].wire, &w, norm) ? 0 : w.nw;
            out_printf(o, "  ch%d: %s, point #%06lX x%u.%02u, max %u%%; %u W output%s per pixel (wire %s)\n", i,
                       settings_wmode_name(g_set.wmode[i]), (unsigned long)g_set.wpoint[i], g_set.wratio[i] / 100,
                       g_set.wratio[i] % 100, g_set.wmax[i], nw, nw == 1 ? "" : "s", g_set.ch[i].wire);
        }
        return 0;
    }
    const char *usage = "usage: white | white <ch> max <0..100> | white <ch> mode <direct|extract|boost> | white <ch> point <#RRGGBB> [ratio 0.1..4]";
    if (argc < 4) { out_puts(o, usage); return 1; }
    char *end;
    long n = strtol(argv[1], &end, 10);
    if (!argv[1][0] || *end || n < 0 || n >= g_set.nch) { out_printf(o, "channel must be 0..%d\n", g_set.nch - 1); return 1; }
    if (!strcmp(argv[2], "max") && argc == 4) {
        long v = strtol(argv[3], &end, 10);
        if (!argv[3][0] || *end || v < 0 || v > 100) { out_puts(o, "max must be 0..100 (percent)"); return 1; }
        g_set.wmax[n] = (uint8_t)v;
    } else if (!strcmp(argv[2], "mode") && argc == 4) {
        int m = settings_wmode_parse(argv[3]);
        if (m < 0) { out_puts(o, "mode is direct, extract or boost"); return 1; }
        g_set.wmode[n] = (uint8_t)m;
    } else if (!strcmp(argv[2], "point") && (argc == 4 || argc == 5)) {
        const char *hex = argv[3][0] == '#' ? argv[3] + 1 : argv[3];
        unsigned long pt = strtoul(hex, &end, 16);
        if (strlen(hex) != 6 || *end || !pt) { out_puts(o, "point must be a color #RRGGBB (not black), e.g. #FFB46B"); return 1; }
        unsigned g = 100;
        if (argc == 5) {                                  /* ratio: like gamma, 1..2 decimals, 0.1..4 */
            unsigned whole = 0, frac = 0, digits = 0;
            const char *q = argv[4];
            while (*q >= '0' && *q <= '9' && whole < 10) whole = whole * 10 + (unsigned)(*q++ - '0');
            if (*q == '.') for (q++; *q >= '0' && *q <= '9' && digits < 2; digits++) frac = frac * 10 + (unsigned)(*q++ - '0');
            if (digits == 1) frac *= 10;
            g = whole * 100 + frac;
            if (*q || q == argv[4] || g < 10 || g > 400) { out_puts(o, "ratio must be 0.1..4 (how bright full W is next to the point color)"); return 1; }
        }
        g_set.wpoint[n] = (uint32_t)pt;
        g_set.wratio[n] = (uint16_t)g;
    } else { out_puts(o, usage); return 1; }
    render_output_update((int)n);
    out_puts(o, "ok (save to persist)");
    return 0;
}

/* output | output refresh <1..4> | output parallel <on|off>: board-wide output stage, applies at once */
static int cmd_output(cmd_out_t *o, int argc, char **argv)
{
    char why[120];
    if (argc == 3 && !strcmp(argv[1], "refresh")) {
        char *end;
        long r = strtol(argv[2], &end, 10);
        if (!argv[2][0] || *end) r = 0;
        if (render_check_output((unsigned)(r < 0 ? 0 : r), g_set.out_parallel, why, sizeof why)) { out_puts(o, why); return 1; }
        g_set.refresh = (uint8_t)r;
    } else if (argc == 3 && !strcmp(argv[1], "parallel") && (!strcmp(argv[2], "on") || !strcmp(argv[2], "off"))) {
        bool on = !strcmp(argv[2], "on");
        if (render_check_output(g_set.refresh, on, why, sizeof why)) { out_printf(o, "%s; lower the refresh first\n", why); return 1; }
        g_set.out_parallel = on;
    } else if (argc != 1) {
        out_puts(o, "usage: output | output refresh <1..4> | output parallel <on|off>");
        return 1;
    }
    if (argc > 1) { render_output_config(); out_puts(o, "ok (save to persist)"); return 0; }
    uint32_t send = 0;
    bool together = led_out_concurrent();
    for (int i = 0; i < g_nchannels; i++) {
        uint32_t t = led_out_send_us(i);
        send = together ? (t > send ? t : send) : send + t;
    }
    out_printf(o, "driver %s, refresh x%u = %u Hz output (programs run at %d Hz), parallel %s%s\n", led_out_driver(),
               g_set.refresh, FRAME_HZ * g_set.refresh, FRAME_HZ, g_set.out_parallel ? "on" : "off",
               strcmp(led_out_driver(), "rmt") ? " (always, PARLIO)" : "");
    out_printf(o, "sending all strings takes ~%lu us; at x%u a tick is %lu us\n", (unsigned long)send, g_set.refresh,
               (unsigned long)(1000000u / (FRAME_HZ * g_set.refresh)));
    return 0;
}

/* Output slot by number or name, or -1 (with a message). */
static int io_arg(cmd_out_t *o, const char *s)
{
    char *end;
    long n = strtol(s, &end, 10);
    if (s[0] && !*end) {
        if (n >= 0 && n < IO_MAX && io_used((int)n)) return (int)n;
        out_printf(o, "no output %s (io to list them)\n", s);
        return -1;
    }
    int k = io_find(s);
    if (k < 0) out_printf(o, "no output named %s (io to list them)\n", s);
    return k;
}

/* io | io <n|name> <on|off|0..1> | io config <n> <gpio> <binary|pwm> [freq=<Hz>] [invert] [name=<name>] | io config <n> off
 * GPIO outputs (relays, PWM). Values apply at once and aren't saved (outputs are always off at boot); the
 * configuration applies at once and `save` keeps it. Outputs whose setup didn't change keep running. */
static int cmd_io(cmd_out_t *o, int argc, char **argv)
{
    if (argc == 1) {
        int shown = 0;
        for (int i = 0; i < IO_MAX; i++) {
            const io_cfg_t *c = &g_set.io[i];
            if (c->gpio < 0) continue;
            shown++;
            out_printf(o, "  %d%s%s: gpio %d, %s", i, c->name[0] ? " " : "", c->name, c->gpio, c->mode == IO_PWM ? "pwm" : "binary");
            if (c->mode == IO_PWM) out_printf(o, " %u Hz", c->freq);
            if (c->invert) out_printf(o, ", inverted");
            uint16_t v = io_get(i);
            if (!io_working(i)) out_printf(o, ": not working (see the log)\n");
            else if (c->mode == IO_BINARY) out_printf(o, ": %s\n", v ? "on" : "off");
            else { unsigned p = (v * 100u + 128u) / 256u; out_printf(o, ": %u.%02u\n", p / 100, p % 100); }
        }
        if (!shown) out_puts(o, "no outputs configured (io config <n> <gpio> <binary|pwm>)");
        return 0;
    }
    const char *usage = "usage: io | io <n|name> <on|off|0..1> | io config <n> <gpio> <binary|pwm> [freq=<Hz>] [invert] [name=<name>] | io config <n> off";
    if (!strcmp(argv[1], "config")) {
        if (argc < 4) { out_puts(o, usage); return 1; }
        char *end;
        long n = strtol(argv[2], &end, 10);
        if (!argv[2][0] || *end || n < 0 || n >= IO_MAX) { out_printf(o, "output must be 0..%d\n", IO_MAX - 1); return 1; }
        io_cfg_t io[IO_MAX];
        memcpy(io, g_set.io, sizeof io);
        io_cfg_t *c = &io[n];
        if (argc == 4 && !strcmp(argv[3], "off")) {
            memset(c, 0, sizeof *c);
            c->gpio = -1;
            c->freq = IO_PWM_DEFAULT_HZ;
        } else {
            if (argc < 5) { out_puts(o, usage); return 1; }
            long gpio = strtol(argv[3], &end, 10);
            if (!argv[3][0] || *end || gpio < 0 || gpio > 127) { out_puts(o, "gpio must be a number"); return 1; }
            io_cfg_t nc;
            memset(&nc, 0, sizeof nc);
            nc.gpio = (int8_t)gpio;
            nc.freq = c->gpio >= 0 && c->mode == IO_PWM ? c->freq : IO_PWM_DEFAULT_HZ;
            if (!strcmp(argv[4], "binary")) nc.mode = IO_BINARY;
            else if (!strcmp(argv[4], "pwm")) nc.mode = IO_PWM;
            else { out_puts(o, "mode is binary or pwm"); return 1; }
            for (int k = 5; k < argc; k++) {
                if (!strcmp(argv[k], "invert")) nc.invert = 1;
                else if (!strncmp(argv[k], "freq=", 5)) {
                    long f = strtol(argv[k] + 5, &end, 10);
                    if (!argv[k][5] || *end || f < IO_FREQ_MIN || f > IO_FREQ_MAX) { out_puts(o, "freq must be 1..20000 (Hz)"); return 1; }
                    nc.freq = (uint16_t)f;
                } else if (!strncmp(argv[k], "name=", 5)) {
                    const char *why = io_check_name(argv[k] + 5);
                    if (why) { out_puts(o, why); return 1; }
                    snprintf(nc.name, sizeof nc.name, "%s", argv[k] + 5);
                } else { out_printf(o, "unknown option %s (freq=<Hz>, invert, name=<name>)\n", argv[k]); return 1; }
            }
            *c = nc;
        }
        int bad;
        const char *why = io_check(io, &bad);
        if (why) { out_printf(o, "output %d: %s\n", bad, why); return 1; }
        memcpy(g_set.io, io, sizeof io);
        io_apply();
        out_puts(o, "ok (save to persist)");
        return 0;
    }
    if (argc != 3) { out_puts(o, usage); return 1; }
    int n = io_arg(o, argv[1]);
    if (n < 0) return 1;
    int v = !strcmp(argv[2], "on") ? UNIT_ONE : !strcmp(argv[2], "off") ? 0 : parse_unit(argv[2]);
    if (v < 0) { out_puts(o, "value is on, off or 0..1"); return 1; }
    const char *why = io_set(n, (uint16_t)v);
    if (why) { out_puts(o, why); return 1; }
    out_puts(o, "ok");
    return 0;
}

#if CONFIG_KTSN_WIFI
/* wifi | wifi off | wifi <ssid> [password]   (quote an SSID with spaces: wifi "My Net" secret) */
static int cmd_wifi(cmd_out_t *o, int argc, char **argv)
{
    if (argc == 1) { net_print_status(o); return 0; }
    if (argc == 2 && !strcmp(argv[1], "off")) {
        g_set.ssid[0] = g_set.pass[0] = 0;
        net_wifi_apply();
        out_puts(o, "ok (save to persist)");
        return 0;
    }
    if (argc > 3) { out_puts(o, "usage: wifi <ssid> [password]  (quote an SSID with spaces)"); return 1; }
    const char *pass = argc == 3 ? argv[2] : "";
    const char *why = settings_check_wifi(argv[1], pass);
    if (why) { out_puts(o, why); return 1; }
    strcpy(g_set.ssid, argv[1]);
    strcpy(g_set.pass, pass);
    net_wifi_apply();
    out_puts(o, "ok (save to persist; check progress with: wifi)");
    return 0;
}

/* ntp | ntp <server> */
static int cmd_ntp(cmd_out_t *o, int argc, char **argv)
{
    if (argc == 2) {
        if (strlen(argv[1]) >= sizeof g_set.ntp) { out_puts(o, "server name too long"); return 1; }
        strcpy(g_set.ntp, argv[1]);
        net_ntp_apply();
        out_puts(o, "ok (save to persist)");
    }
    net_print_status(o);
    return 0;
}

/* token | token <secret> | token off. The token is never shown, over any transport. */
static int cmd_token(cmd_out_t *o, int argc, char **argv)
{
    if (argc == 1) {
        out_puts(o, g_set.token[0] ? "token is set (network access needs: auth <token>)"
                                   : "no token: network access is disabled (set one with: token <secret>)");
        return 0;
    }
    if (argc != 2) { out_puts(o, "usage: token <secret> | token off"); return 1; }
    if (!strcmp(argv[1], "off")) {
        g_set.token[0] = 0;
        out_puts(o, "token cleared: network access disabled (save to persist)");
        return 0;
    }
    const char *why = settings_check_token(argv[1]);
    if (why) { out_puts(o, why); return 1; }
    strcpy(g_set.token, argv[1]);
    out_puts(o, "ok (save to persist)");
    return 0;
}
#endif

#if CONFIG_KTSN_WIFI
/* hostname | hostname <name> */
static int cmd_hostname(cmd_out_t *o, int argc, char **argv)
{
    if (argc == 2) {
        const char *why = settings_check_hostname(argv[1]);
        if (why) { out_puts(o, why); return 1; }
        strcpy(g_set.hostname, argv[1]);
        net_hostname_apply();
    }
    out_printf(o, "hostname %s (%s.local)%s\n", g_set.hostname, g_set.hostname, argc == 2 ? " (save to persist)" : "");
    return 0;
}
#endif

static void print_deg(cmd_out_t *o, int32_t v)      /* 1e-5 degrees */
{
    long a = labs((long)v);
    out_printf(o, "%s%ld.%05ld", v < 0 ? "-" : "", a / 100000, a % 100000);
}

static void print_hhmm(cmd_out_t *o, const char *name, int m)
{
    if (m < 0) out_printf(o, "%s none", name);
    else out_printf(o, "%s %02d:%02d", name, m / 60, m % 60);
}

/* loc | loc <lat> <lon> | loc off: where the board is, for env sunrise, sunset, dawn, dusk */
static int cmd_loc(cmd_out_t *o, int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "off")) g_set.has_loc = false;
    else if (argc == 3) {
        char *e1, *e2;
        double lat = strtod(argv[1], &e1), lon = strtod(argv[2], &e2);
        if (e1 == argv[1] || *e1 || e2 == argv[2] || *e2) { out_puts(o, "usage: loc <lat> <lon>, in degrees, north and east positive"); return 1; }
        const char *why = settings_check_loc(lat, lon);
        if (why) { out_puts(o, why); return 1; }
        settings_set_loc(lat, lon);
    } else if (argc != 1) { out_puts(o, "usage: loc | loc <lat> <lon> | loc off"); return 1; }
    if (argc > 1) vTaskDelay(pdMS_TO_TICKS(250));        /* the automation task picks it up within 100 ms */
    if (g_set.has_loc) { out_printf(o, "loc "); print_deg(o, g_set.lat); out_printf(o, " "); print_deg(o, g_set.lon); out_printf(o, "\n"); }
    else out_puts(o, "loc not set (loc <lat> <lon>)");
    print_hhmm(o, "dawn", g_env.dawn); print_hhmm(o, ", sunrise", g_env.sunrise);
    print_hhmm(o, ", sunset", g_env.sunset); print_hhmm(o, ", dusk", g_env.dusk);
    out_printf(o, "\nmoon %d deg, %d%% lit (computed in %lu us)\n", g_env.moon, g_env.moonlit, (unsigned long)sky_last_us());
    return 0;
}

/* tz | tz <POSIX TZ> */
static int cmd_tz(cmd_out_t *o, int argc, char **argv)
{
    if (argc == 2) {
        if (strlen(argv[1]) >= sizeof g_set.tz) { out_puts(o, "TZ string too long"); return 1; }
        strcpy(g_set.tz, argv[1]);
        settings_apply_tz();
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    out_printf(o, "tz %s, local time %04d-%02d-%02d %02d:%02d:%02d%s\n", g_set.tz, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
               tm.tm_hour, tm.tm_min, tm.tm_sec, argc == 2 ? " (save to persist)" : "");
    return 0;
}

static int cmd_save(cmd_out_t *o, int argc, char **argv)
{
    (void)argc; (void)argv;
    esp_err_t r = settings_save();
    if (r != ESP_OK) { out_printf(o, "save failed: %s\n", esp_err_to_name(r)); return 1; }
    out_puts(o, "saved (see: config)");
    return 0;
}

static int cmd_config(cmd_out_t *o, int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "erase")) {
        esp_err_t r = settings_erase();
        if (r != ESP_OK) { out_printf(o, "erase failed: %s\n", esp_err_to_name(r)); return 1; }
        out_puts(o, "stored configuration and programs erased (the program library is kept: lib rm); defaults apply after reboot");
        return 0;
    }
    if (argc != 1) { out_puts(o, "usage: config [erase]"); return 1; }
    settings_dump(o);
    return 0;
}

static int cmd_reboot(cmd_out_t *o, int argc, char **argv)
{
    (void)argc; (void)argv;
    out_puts(o, "rebooting");
    esp_restart();
    return 0;
}

static int cmd_help(cmd_out_t *o, int argc, char **argv)
{
    (void)argc; (void)argv;
    for (int i = 0; i < g_ncmds; i++) out_printf(o, "  %-9s %s\n", g_cmds[i].name, g_cmds[i].help);
#if CONFIG_KTSN_SERIAL_API
    out_puts(o, "  api       Machine-readable JSON mode for tools such as the IDE (serial console only)");
#endif
    return 0;
}

const cmd_def_t g_cmds[] = {
    { "help",     "List commands", cmd_help },
    { "prog",     "Set render program: prog <ch> (fill (buf O) #00FF00) (scale (buf O) 0.5) | prog <ch>  (several lines, end with .)", cmd_prog },
    { "list",     "Show program source: list [ch]", cmd_list },
    { "env",      "Show env, or: env intensity <0..256> (global; see also intensity) | env time YYYY-MM-DD HH:MM:SS", cmd_env },
    { "buf",      "Dump pixels: buf <ch> <X|Y|Z|O|G0..G7> [start] [count]", cmd_buf },
    { "stat",     "Performance stats, or: stat reset", cmd_stat },
    { "autoexec", "Boot-time program (runs once, for global buffers): autoexec [run | clear | paste | <source>]", cmd_autoexec },
    { "chan",     "Channel layout: chan | chan <n> <gpio> <npix> [wire, e.g. GRB or \"BRG WWW\"] | chan <n> off  (save + reboot)", cmd_chan },
    { "gamma",    "Gamma correction at output: gamma | gamma <ch> <1.0..3.0 | off>  (applies at once)", cmd_gamma },
    { "dither",   "Temporal dither at output: dither | dither <ch> <0..4 bits, 0 = off>  (applies at once)", cmd_dither },
    { "version",  "Firmware version, running slot (and whether it is confirmed), the other slot", cmd_version },
    { "automation", "Automation: automation | automation <source> | paste | clear | run | every <seconds>", cmd_automation },
    { "intensity", "Intensity: intensity | intensity <ch|all> <0..1> [fade seconds]  (all = global; applies at once)", cmd_intensity },
    { "lib",      "Program library: lib | lib show|rm <name> | lib set <name> [source] | lib save|run <name> <ch>", cmd_lib },
    { "white",    "White (W): white | white <ch> max <0..100> | mode <direct|extract|boost> | point <#RRGGBB> [ratio]  (at once)", cmd_white },
    { "io",       "GPIO outputs: io | io <n|name> <on|off|0..1> | io config <n> <gpio> <binary|pwm> [freq=Hz] [invert] [name=x] | io config <n> off", cmd_io },
    { "output",   "Output stage: output | output refresh <1..4> (sends per frame) | output parallel <on|off>  (at once)", cmd_output },
#if CONFIG_KTSN_WIFI
    { "wifi",     "WiFi: wifi | wifi <ssid> [password] | wifi off", cmd_wifi },
    { "ntp",      "NTP: ntp | ntp <server>", cmd_ntp },
    { "token",    "Network access token: token | token <secret> | token off", cmd_token },
    { "hostname", "Network name (DHCP, <name>.local): hostname | hostname <name>", cmd_hostname },
#endif
    { "tz",       "Time zone: tz | tz <POSIX TZ>, e.g. tz PST8PDT,M3.2.0,M11.1.0", cmd_tz },
    { "loc",      "Location for sunrise and sunset: loc | loc <lat> <lon> | loc off  (degrees, north and east positive)", cmd_loc },
    { "save",     "Save programs, autoexec, automation, channels, outputs, gamma, dither, white, output, intensity, tz, loc (and wifi, ntp, token, hostname) to flash", cmd_save },
    { "config",   "Show what is saved in flash, or: config erase", cmd_config },
    { "reboot",   "Restart the controller", cmd_reboot },
};
const int g_ncmds = sizeof g_cmds / sizeof g_cmds[0];

/* Serial REPL: every table entry is registered with esp_console, with the entry itself as context. */
static int serial_cmd(void *ctx, int argc, char **argv)
{
    const cmd_def_t *d = ctx;
    cmd_lock();
    g_out_stdout.line = cmd_serial_line();
    int r = d->fn(&g_out_stdout, argc, argv);
    g_out_stdout.line = NULL;
    cmd_unlock();
    return r;
}

void console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "led> ";
    rc.max_cmdline_length = CMD_LINE_MAX;
    rc.max_cmdline_args = CMD_MAX_ARGS;         /* default 32 silently cut long programs */
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    /* Boards whose USB port is the chip's own USB-Serial/JTAG (e.g. XIAO ESP32-C6): preset sdkconfig.usbjtag */
    esp_console_dev_usb_serial_jtag_config_t hw = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw, &rc, &repl));
    /* The REPL installs the USB-Serial/JTAG driver with a 256-byte RX buffer, and the driver silently drops what
     * doesn't fit (no USB backpressure): a long serial-API request arrives in a few ms and got truncated. Swap in a
     * driver that holds a whole request. Writes go straight to the hardware while the driver is swapped. */
    usb_serial_jtag_vfs_use_nonblocking();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_uninstall());
    usb_serial_jtag_driver_config_t uj = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    uj.rx_buffer_size = API_LINE_MAX + 1024;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&uj));
    usb_serial_jtag_vfs_use_driver();
#else
    esp_console_dev_uart_config_t hw = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw, &rc, &repl));
#endif
    for (int i = 0; i < g_ncmds; i++) {
        const esp_console_cmd_t c = { .command = g_cmds[i].name, .help = g_cmds[i].help,
                                      .func_w_context = serial_cmd, .context = (void *)&g_cmds[i] };
        ESP_ERROR_CHECK(esp_console_cmd_register(&c));
    }
#if CONFIG_KTSN_SERIAL_API
    serial_api_register();
#endif
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
