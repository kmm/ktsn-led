/* Transport-neutral JSON API (see api.h). Moved out of http_api.c so the serial API shares every line of it.
 * Settings and channels are only touched under cmd_lock, like every console command. cJSON uses doubles: fine here
 * (config path only), never in the render path. */
#include "api.h"
#include "antenna.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "app.h"
#include "cmd.h"
#include "dither.h"
#include "gamma.h"
#include "io.h"
#include "led_out.h"
#include "automation.h"
#include "firmware.h"
#include "progs.h"
#include "render.h"
#include "settings.h"
#include "sdkconfig.h"
#if CONFIG_KTSN_WIFI
#include "net.h"
#endif
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static api_result_t result(int status, cJSON *body) { return (api_result_t){ .status = status, .body = body }; }

api_result_t api_error(int status, const char *msg)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "error", msg);
    return result(status, j);
}

static api_result_t ok(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    return result(200, j);
}

static void add_stat(cJSON *parent, const char *name, const stat_us_t *s)
{
    cJSON *o = cJSON_AddObjectToObject(parent, name);
    cJSON_AddNumberToObject(o, "last", s->last);
    cJSON_AddNumberToObject(o, "avg", s->avg);
    cJSON_AddNumberToObject(o, "max", s->max);
}

static void add_time(cJSON *parent, const char *name, time_t t)
{
    struct tm tm;
    char buf[24];
    localtime_r(&t, &tm);
    strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
    cJSON_AddStringToObject(parent, name, buf);
}

static bool reboot_needed(void)
{
    if (g_nchannels != g_set.nch) return true;
    for (int i = 0; i < g_nchannels; i++)
        if (g_channels[i].npix != g_set.ch[i].npix || strcmp(led_out_wire(i), g_set.ch[i].wire)) return true;
    return false;
}

/* ---------------- status ---------------- */

api_result_t api_status(void)
{
#if CONFIG_KTSN_WIFI
    net_status_t ns;
    net_get_status(&ns);
#endif
    cJSON *j = cJSON_CreateObject();
    cmd_lock();
    int64_t now_us = esp_timer_get_time(), el = now_us - g_perf.t0_us;
    cJSON_AddNumberToObject(j, "uptime_s", (double)(now_us / 1000000));
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON *fw = cJSON_AddObjectToObject(j, "firmware");
    cJSON_AddStringToObject(fw, "version", app->version);
    cJSON_AddStringToObject(fw, "built", app->date);                    /* "Oct 17 2026" */
    cJSON_AddStringToObject(fw, "time", app->time);
    cJSON_AddStringToObject(fw, "idf", app->idf_ver);
    cJSON_AddStringToObject(fw, "target", CONFIG_IDF_TARGET);
    cJSON_AddStringToObject(fw, "driver", led_out_driver());
    fw_info_t fi;
    fw_info(&fi);
    cJSON_AddStringToObject(fw, "slot", fi.slot);
    cJSON_AddStringToObject(fw, "state", fi.state);                    /* "pending" = new, not confirmed yet (rollback) */
    if (fi.has_other && fi.other_valid) {
        cJSON *ot = cJSON_AddObjectToObject(fw, "other");
        cJSON_AddStringToObject(ot, "slot", fi.other_slot);
        cJSON_AddStringToObject(ot, "version", fi.other_version);
        cJSON_AddStringToObject(ot, "built", fi.other_date);
        cJSON_AddStringToObject(ot, "time", fi.other_time);
    } else cJSON_AddNullToObject(fw, "other");
#if CONFIG_KTSN_OTA
    cJSON_AddBoolToObject(fw, "ota", esp_ota_get_next_update_partition(NULL) != NULL);   /* POST /api/ota works */
#else
    cJSON_AddBoolToObject(fw, "ota", false);
#endif
    cJSON_AddNumberToObject(j, "fps", el > 0 ? (double)((int64_t)g_perf.frames * 100000000LL / el) / 100.0 : 0);
    cJSON_AddNumberToObject(j, "frames", g_perf.frames);
    cJSON_AddNumberToObject(j, "output_hz", el > 0 ? (double)((int64_t)g_perf.sends * 100000000LL / el) / 100.0 : 0);
    cJSON_AddNumberToObject(j, "sends", g_perf.sends);
    cJSON_AddNumberToObject(j, "missed", g_perf.missed);              /* timer ticks */
    cJSON_AddNumberToObject(j, "intensity", render_intensity(INTENSITY_GLOBAL) / 256.0);   /* now, 0..1 (mid-fade too) */
    automation_status_t as;
    automation_get_status(&as);
    cJSON *au = cJSON_AddObjectToObject(j, "automation");
    cJSON_AddBoolToObject(au, "active", as.active);
    cJSON_AddNumberToObject(au, "every", g_set.auto_every);
    cJSON_AddNumberToObject(au, "runs", as.runs);
    if (as.last_run < 0) cJSON_AddNullToObject(au, "last_run");
    else if (as.last_run_is_time) add_time(au, "last_run", (time_t)as.last_run);
    else cJSON_AddNumberToObject(au, "last_run_uptime_s", (double)as.last_run);
    cJSON_AddNumberToObject(au, "faults", as.faults);
    if (as.last_err) cJSON_AddStringToObject(au, "last_error", as.last_err);
    else cJSON_AddNullToObject(au, "last_error");
    cJSON *ios = cJSON_AddArrayToObject(j, "io");
    for (int i = 0; i < IO_MAX; i++) {
        if (g_set.io[i].gpio < 0) continue;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "n", i);
        cJSON_AddStringToObject(e, "name", g_set.io[i].name);
        cJSON_AddStringToObject(e, "mode", io_mode_name(g_set.io[i].mode));
        if (g_set.io[i].mode == IO_ANALOG)                                               /* 0..1, 16 readings */
            cJSON_AddNumberToObject(e, "value", (double)io_analog_get(i, DSL_ANALOG_SAMPLES_DEFAULT) / IO_ANALOG_FULL);
        else cJSON_AddNumberToObject(e, "value", io_get(i) / 256.0);                    /* 0..1 (binary, input: 0 or 1) */
        cJSON_AddBoolToObject(e, "ok", io_working(i));
        cJSON_AddItemToArray(ios, e);
    }
    io_audio_stat_t aus;
    io_audio_stat(&aus);
    if (aus.slot < 0) cJSON_AddNullToObject(j, "audio");
    else {
        cJSON *a = cJSON_AddObjectToObject(j, "audio");
        cJSON_AddNumberToObject(a, "n", aus.slot);
        double bands[AUDIO_BANDS];
        for (int b = 0; b < AUDIO_BANDS; b++) bands[b] = (double)aus.band[b] / AUDIO_ONE;
        cJSON_AddItemToObject(a, "bands", cJSON_CreateDoubleArray(bands, AUDIO_BANDS));
        cJSON_AddNumberToObject(a, "level", (double)aus.level / AUDIO_ONE);
        cJSON_AddNumberToObject(a, "beats", aus.beats);
        cJSON_AddNumberToObject(a, "beatms", io_audio_beatms());
        cJSON_AddNumberToObject(a, "swing", aus.swing);
        cJSON_AddNumberToObject(a, "rms", aus.rms);
        cJSON_AddNumberToObject(a, "adc_hz", aus.adc_hz);
        cJSON_AddNumberToObject(a, "blocks", aus.blocks);
        cJSON_AddNumberToObject(a, "us_avg", aus.us_avg);
        cJSON_AddNumberToObject(a, "us_max", aus.us_max);
        cJSON_AddNumberToObject(a, "overflows", aus.dropped);
    }
    add_stat(j, "frame_us", &g_perf.frame);
    add_stat(j, "flush_us", &g_perf.flush);
    cJSON_AddNumberToObject(j, "render_stack_free", render_stack_free());
    cJSON *chs = cJSON_AddArrayToObject(j, "channels");
    for (int i = 0; i < g_nchannels; i++) {
        const channel_t *ch = &g_channels[i];
        cJSON *c = cJSON_CreateObject();
        cJSON_AddNumberToObject(c, "npix", ch->npix);
        cJSON_AddNumberToObject(c, "nodes", ch->perf.nodes_last);
        cJSON_AddNumberToObject(c, "stack_est", ch->perf.stack);
        cJSON_AddNumberToObject(c, "faults", ch->perf.faults);
        cJSON_AddNumberToObject(c, "intensity", render_intensity(i) / 256.0);              /* now, 0..1 */
        if (ch->perf.last_err) cJSON_AddStringToObject(c, "last_error", ch->perf.last_err);
        else cJSON_AddNullToObject(c, "last_error");
        add_stat(c, "render_us", &ch->perf.render);
        add_stat(c, "output_us", &ch->perf.output);
        cJSON_AddItemToArray(chs, c);
    }
    add_time(j, "time", time(NULL));
    cJSON_AddStringToObject(j, "tz", g_set.tz);
    cJSON_AddStringToObject(j, "hostname", g_set.hostname);
#if CONFIG_KTSN_WIFI
    cJSON *w = cJSON_AddObjectToObject(j, "wifi");
    cJSON_AddStringToObject(w, "ssid", g_set.ssid);
    cJSON_AddBoolToObject(w, "connected", ns.connected);
    cJSON_AddStringToObject(w, "ip", ns.ip);
    cJSON_AddNumberToObject(w, "rssi", ns.rssi);
    cJSON *n = cJSON_AddObjectToObject(j, "ntp");
    cJSON_AddStringToObject(n, "server", g_set.ntp);
    cJSON_AddBoolToObject(n, "synced", ns.ntp_synced);
    if (ns.ntp_synced) add_time(n, "last_sync", ns.last_sync);
    else cJSON_AddNullToObject(n, "last_sync");
#else
    cJSON_AddNullToObject(j, "wifi");                     /* WiFi-less build */
#endif
    cmd_unlock();
    cJSON_AddNumberToObject(j, "heap_free", esp_get_free_heap_size());
    return result(200, j);
}

/* ---------------- config ---------------- */

api_result_t api_get_config(void)
{
    cJSON *j = cJSON_CreateObject();
    cmd_lock();
    cJSON *chs = cJSON_AddArrayToObject(j, "channels");
    for (int i = 0; i < g_set.nch; i++) {
        cJSON *c = cJSON_CreateObject();
        cJSON_AddNumberToObject(c, "gpio", g_set.ch[i].gpio);
        cJSON_AddNumberToObject(c, "npix", g_set.ch[i].npix);
        cJSON_AddStringToObject(c, "wire", g_set.ch[i].wire);
        cJSON_AddNumberToObject(c, "gamma", g_set.gamma[i] / 100.0);     /* 1 = off */
        cJSON_AddNumberToObject(c, "dither", g_set.dither[i]);           /* bits, 0 = off */
        cJSON_AddNumberToObject(c, "wmax", g_set.wmax[i]);               /* white limit, percent */
        cJSON_AddNumberToObject(c, "intensity", (i < g_nchannels ? render_intensity_target(i) : g_set.cintens[i]) / 256.0);   /* 0..1 */
        cJSON_AddStringToObject(c, "wmode", settings_wmode_name(g_set.wmode[i]));
        char pt[8];
        snprintf(pt, sizeof pt, "#%06lX", (unsigned long)g_set.wpoint[i]);
        cJSON_AddStringToObject(c, "wpoint", pt);
        cJSON_AddNumberToObject(c, "wratio", g_set.wratio[i] / 100.0);
        cJSON_AddBoolToObject(c, "running", i < g_nchannels);
        if (i < g_nchannels && g_channels[i].source) cJSON_AddStringToObject(c, "prog", g_channels[i].source);
        else cJSON_AddNullToObject(c, "prog");
        cJSON_AddItemToArray(chs, c);
    }
    if (g_set.autoexec) cJSON_AddStringToObject(j, "autoexec", g_set.autoexec);
    else cJSON_AddNullToObject(j, "autoexec");
    if (g_set.automation) cJSON_AddStringToObject(j, "automation", g_set.automation);
    else cJSON_AddNullToObject(j, "automation");
    cJSON_AddNumberToObject(j, "automation_every", g_set.auto_every);
    cJSON_AddNumberToObject(j, "audio_gate", g_set.audio_gate);           /* RMS in ADC counts, 0 = off */
    cJSON_AddNumberToObject(j, "intensity", render_intensity_target(INTENSITY_GLOBAL));   /* 0..256 (target if fading) */
    cJSON *ios = cJSON_AddArrayToObject(j, "io");                          /* configured outputs only */
    for (int i = 0; i < IO_MAX; i++) {
        const io_cfg_t *c = &g_set.io[i];
        if (c->gpio < 0) continue;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "n", i);
        cJSON_AddNumberToObject(e, "gpio", c->gpio);
        cJSON_AddStringToObject(e, "mode", io_mode_name(c->mode));
        cJSON_AddNumberToObject(e, "freq", c->freq);
        cJSON_AddBoolToObject(e, "invert", c->invert);
        cJSON_AddStringToObject(e, "name", c->name);
        cJSON_AddStringToObject(e, "pull", io_pull_name(c->pull));
        cJSON_AddNumberToObject(e, "debounce", c->debounce);
        cJSON_AddBoolToObject(e, "trigger", c->trigger);
        cJSON_AddItemToArray(ios, e);
    }
    cJSON_AddNumberToObject(j, "io_max", IO_MAX);
    cJSON *out = cJSON_AddObjectToObject(j, "output");
    cJSON_AddStringToObject(out, "driver", led_out_driver());
    cJSON_AddNumberToObject(out, "refresh", g_set.refresh);            /* sends per frame, 1..4 */
    cJSON_AddBoolToObject(out, "parallel", g_set.out_parallel);        /* RMT only; PARLIO always sends together */
    cJSON_AddStringToObject(j, "tz", g_set.tz);
    cJSON_AddStringToObject(j, "ntp", g_set.ntp);
    if (g_set.has_loc) {
        cJSON *l = cJSON_AddObjectToObject(j, "loc");
        cJSON_AddNumberToObject(l, "lat", g_set.lat / 1e5);
        cJSON_AddNumberToObject(l, "lon", g_set.lon / 1e5);
    } else cJSON_AddNullToObject(j, "loc");
    cJSON *w = cJSON_AddObjectToObject(j, "wifi");
    cJSON_AddStringToObject(w, "ssid", g_set.ssid);
    cJSON_AddBoolToObject(w, "password_set", g_set.pass[0] != 0);
    cJSON_AddStringToObject(j, "hostname", g_set.hostname);
#if CONFIG_KTSN_XIAO_C6_ANTENNA                        /* only in builds with the switch: the IDE shows it when present */
    cJSON_AddStringToObject(j, "antenna", g_set.antenna_ext ? "external" : "internal");
#endif
    cJSON_AddBoolToObject(j, "token_set", g_set.token[0] != 0);
    cJSON_AddBoolToObject(j, "reboot_needed", reboot_needed());
    cmd_unlock();
    return result(200, j);
}

static bool json_int(const cJSON *v, long lo, long hi, long *out)
{
    if (!cJSON_IsNumber(v) || v->valuedouble != (double)(long)v->valuedouble) return false;
    long x = (long)v->valuedouble;
    if (x < lo || x > hi) return false;
    *out = x;
    return true;
}

static void err_at(cJSON *errs, const char *key, const char *msg) { cJSON_AddStringToObject(errs, key, msg); }

/* Copy a string field into dst (cap bytes) or record an error. */
static bool json_str(cJSON *errs, const char *key, const cJSON *v, char *dst, size_t cap, bool allow_empty)
{
    if (!cJSON_IsString(v)) { err_at(errs, key, "must be a string"); return false; }
    if (!allow_empty && !v->valuestring[0]) { err_at(errs, key, "must not be empty"); return false; }
    if (strlen(v->valuestring) >= cap) { err_at(errs, key, "too long"); return false; }
    strcpy(dst, v->valuestring);
    return true;
}

#if CONFIG_KTSN_WIFI
static void apply_wifi_after(void)                       /* after the response: this may drop our own connection */
{
    cmd_lock();
    net_wifi_apply();
    cmd_unlock();
}
#endif

api_result_t api_patch_config(const cJSON *root)
{
    if (!cJSON_IsObject(root)) return api_error(400, "body must be a JSON object");

    cJSON *errs = cJSON_CreateObject();
    program_t *progs[CHANNELS_MAX] = { 0 };
    const char *prog_src[CHANNELS_MAX] = { 0 };
    char key[32], err[DSL_ERR_LEN];

    cmd_lock();
    /* Working copies; g_set and the channels are only touched after everything validated. */
    uint8_t nch = g_set.nch;
    led_chan_cfg_t ch[CHANNELS_MAX];
    memcpy(ch, g_set.ch, sizeof ch);
    uint16_t gam[CHANNELS_MAX];
    memcpy(gam, g_set.gamma, sizeof gam);
    uint8_t dith[CHANNELS_MAX], wmx[CHANNELS_MAX], wmo[CHANNELS_MAX];
    uint32_t wpt[CHANNELS_MAX];
    uint16_t wrt[CHANNELS_MAX];
    memcpy(dith, g_set.dither, sizeof dith);
    memcpy(wmx, g_set.wmax, sizeof wmx);
    memcpy(wmo, g_set.wmode, sizeof wmo);
    memcpy(wpt, g_set.wpoint, sizeof wpt);
    memcpy(wrt, g_set.wratio, sizeof wrt);
    long intensity = -1, refresh = g_set.refresh;
    double fade_s = 0;
    double cint[CHANNELS_MAX];                            /* per channel intensity 0..1, < 0 = unchanged */
    for (int i = 0; i < CHANNELS_MAX; i++) cint[i] = -1;
    bool parallel = g_set.out_parallel, output_set = false;
    char tz[sizeof g_set.tz], ntp[sizeof g_set.ntp], ssid[sizeof g_set.ssid], pass[sizeof g_set.pass], token[sizeof g_set.token];
    char host[sizeof g_set.hostname];
    strcpy(tz, g_set.tz); strcpy(ntp, g_set.ntp); strcpy(ssid, g_set.ssid); strcpy(pass, g_set.pass); strcpy(token, g_set.token);
    strcpy(host, g_set.hostname);
    bool loc_set = false, loc_off = false;
#if CONFIG_KTSN_XIAO_C6_ANTENNA
    int antenna = -1;                                     /* 0 internal, 1 external, -1 unchanged */
#endif
    double loc_lat = 0, loc_lon = 0;
    bool tz_set = false, ntp_set = false, wifi_set = false, token_set = false, layout_set = false, save = false, host_set = false;
    int auto_action = 0;                                  /* 1 = set, 2 = clear */
    int automat_action = 0;                               /* automation: 1 = set, 2 = clear */
    const char *automat_src = NULL;
    long auto_every = -1, audio_gate = -1;
    const char *auto_src = NULL;
    io_cfg_t io[IO_MAX];
    memcpy(io, g_set.io, sizeof io);
    bool io_cfg_set = false;
    const cJSON *io_set_req = NULL;

    const cJSON *item;
    cJSON_ArrayForEach(item, root) {
        const char *k = item->string;
        if (!strcmp(k, "intensity")) {
            if (!json_int(item, 0, UNIT_ONE, &intensity)) err_at(errs, k, "must be an integer 0..256");
        } else if (!strcmp(k, "fade")) {                 /* seconds for the intensity changes in this patch */
            if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > FADE_MAX_MS / 1000) err_at(errs, k, "must be 0..3600 (seconds)");
            else fade_s = item->valuedouble;
        } else if (!strcmp(k, "output")) {
            if (!cJSON_IsObject(item)) { err_at(errs, k, "must be an object {refresh, parallel}"); continue; }
            const cJSON *rf = cJSON_GetObjectItemCaseSensitive(item, "refresh"), *pa = cJSON_GetObjectItemCaseSensitive(item, "parallel");
            if (rf && !json_int(rf, 1, REFRESH_MAX, &refresh)) { err_at(errs, "output.refresh", "must be an integer 1..4"); continue; }
            if (pa && !cJSON_IsBool(pa)) { err_at(errs, "output.parallel", "must be true or false"); continue; }
            if (pa) parallel = cJSON_IsTrue(pa);
            char why[120];
            if (render_check_output((unsigned)refresh, parallel, why, sizeof why)) err_at(errs, "output.refresh", why);
            else output_set = true;
        } else if (!strcmp(k, "tz")) {
            tz_set = json_str(errs, k, item, tz, sizeof tz, false);
        } else if (!strcmp(k, "ntp")) {
            ntp_set = json_str(errs, k, item, ntp, sizeof ntp, false);
        } else if (!strcmp(k, "loc")) {
            if (cJSON_IsNull(item)) { loc_set = true; loc_off = true; continue; }
            const cJSON *la = cJSON_GetObjectItemCaseSensitive(item, "lat"), *lo = cJSON_GetObjectItemCaseSensitive(item, "lon");
            if (!cJSON_IsNumber(la) || !cJSON_IsNumber(lo)) { err_at(errs, k, "must be an object {lat, lon} or null (none)"); continue; }
            const char *why = settings_check_loc(la->valuedouble, lo->valuedouble);
            if (why) { err_at(errs, k, why); continue; }
            loc_set = true; loc_off = false; loc_lat = la->valuedouble; loc_lon = lo->valuedouble;
        } else if (!strcmp(k, "wifi")) {
            if (cJSON_IsNull(item)) { ssid[0] = pass[0] = 0; wifi_set = true; continue; }
            if (!cJSON_IsObject(item)) { err_at(errs, k, "must be an object {ssid, password} or null (off)"); continue; }
            const cJSON *s = cJSON_GetObjectItemCaseSensitive(item, "ssid"), *p = cJSON_GetObjectItemCaseSensitive(item, "password");
            bool good = json_str(errs, "wifi.ssid", s, ssid, sizeof ssid, false);
            if (p) good = json_str(errs, "wifi.password", p, pass, sizeof pass, true) && good;   /* omitted: keep current */
            const char *why = good ? settings_check_wifi(ssid, pass) : NULL;
            if (why) err_at(errs, k, why);
            wifi_set = good && !why;
        } else if (!strcmp(k, "hostname")) {
            if (!json_str(errs, k, item, host, sizeof host, false)) continue;
            const char *why = settings_check_hostname(host);
            if (why) err_at(errs, k, why); else host_set = true;
        } else if (!strcmp(k, "antenna")) {
#if CONFIG_KTSN_XIAO_C6_ANTENNA
            if (cJSON_IsString(item) && !strcmp(item->valuestring, "internal")) antenna = 0;
            else if (cJSON_IsString(item) && !strcmp(item->valuestring, "external")) antenna = 1;
            else err_at(errs, k, "must be \"internal\" or \"external\"");
#else
            err_at(errs, k, "this build has no antenna switch (XIAO C6 builds only)");
#endif
        } else if (!strcmp(k, "token")) {
            if (!cJSON_IsString(item)) { err_at(errs, k, "must be a string (clearing it is only possible on the serial console)"); continue; }
            const char *why = settings_check_token(item->valuestring);
            if (why) err_at(errs, k, why);
            else { strcpy(token, item->valuestring); token_set = true; }
        } else if (!strcmp(k, "automation")) {
            char aerr[DSL_ERR_LEN];
            if (cJSON_IsNull(item)) automat_action = 2;
            else if (!cJSON_IsString(item)) err_at(errs, k, "must be a string or null");
            else if (strlen(item->valuestring) > SOURCE_MAX) err_at(errs, k, "too long");
            else if (!item->valuestring[0]) automat_action = 2;
            else if (automation_check(item->valuestring, aerr, sizeof aerr)) err_at(errs, k, aerr);
            else { automat_action = 1; automat_src = item->valuestring; }
        } else if (!strcmp(k, "audio_gate")) {
            if (!json_int(item, 0, AUDIO_GATE_MAX, &audio_gate)) err_at(errs, k, "must be an integer 0..2000 (rms in ADC counts, 0 = off)");
        } else if (!strcmp(k, "automation_every")) {
            if (!json_int(item, 1, AUTO_EVERY_MAX, &auto_every)) err_at(errs, k, "must be an integer 1..3600 (seconds)");
        } else if (!strcmp(k, "autoexec")) {
            if (cJSON_IsNull(item)) auto_action = 2;
            else if (!cJSON_IsString(item)) err_at(errs, k, "must be a string or null");
            else if (strlen(item->valuestring) > SOURCE_MAX) err_at(errs, k, "too long");
            else { auto_action = 1; auto_src = item->valuestring; }
        } else if (!strcmp(k, "io")) {   /* {"0": {gpio, mode, freq, invert, name, pull, debounce, trigger} | null (remove)} */
            if (!cJSON_IsObject(item)) { err_at(errs, k, "must be an object keyed by output number, e.g. {\"0\": {...}}"); continue; }
            const cJSON *c;
            cJSON_ArrayForEach(c, item) {
                char *end;
                long n = strtol(c->string, &end, 10);
                snprintf(key, sizeof key, "io.%s", c->string);
                if (!c->string[0] || *end || n < 0 || n >= IO_MAX) { err_at(errs, key, "unknown output number"); continue; }
                io_cfg_set = true;
                if (cJSON_IsNull(c)) { memset(&io[n], 0, sizeof io[n]); io[n].gpio = -1; io[n].freq = IO_PWM_DEFAULT_HZ; io[n].debounce = IO_DEBOUNCE_DEFAULT; continue; }
                if (!cJSON_IsObject(c)) { err_at(errs, key, "must be an object {gpio, mode, freq, invert, name, pull, debounce, trigger} or null (remove)"); continue; }
                const cJSON *g = cJSON_GetObjectItemCaseSensitive(c, "gpio"), *mo = cJSON_GetObjectItemCaseSensitive(c, "mode");
                const cJSON *fq = cJSON_GetObjectItemCaseSensitive(c, "freq"), *iv = cJSON_GetObjectItemCaseSensitive(c, "invert");
                const cJSON *nm = cJSON_GetObjectItemCaseSensitive(c, "name"), *pl = cJSON_GetObjectItemCaseSensitive(c, "pull");
                const cJSON *db = cJSON_GetObjectItemCaseSensitive(c, "debounce"), *tr = cJSON_GetObjectItemCaseSensitive(c, "trigger");
                io_cfg_t nc = io[n];
                long v;
                if (nc.gpio < 0 && !g) { err_at(errs, key, "a new slot needs a gpio"); continue; }
                if (g) { if (!json_int(g, 0, 63, &v)) { snprintf(key, sizeof key, "io.%s.gpio", c->string); err_at(errs, key, "must be an integer"); continue; } nc.gpio = (int8_t)v; }
                if (mo) {
                    snprintf(key, sizeof key, "io.%s.mode", c->string);
                    int m = cJSON_IsString(mo) ? io_mode_parse(mo->valuestring) : -1;
                    if (m < 0) { err_at(errs, key, "must be \"binary\", \"pwm\", \"input\", \"analog\" or \"audio\""); continue; }
                    if (m == IO_INPUT && nc.mode != IO_INPUT && !pl) nc.pull = IO_PULL_UP;   /* a new input: pull up unless told */
                    nc.mode = (uint8_t)m;
                }
                if (pl) {
                    snprintf(key, sizeof key, "io.%s.pull", c->string);
                    int p = cJSON_IsString(pl) ? io_pull_parse(pl->valuestring) : -1;
                    if (p < 0) { err_at(errs, key, "must be \"up\", \"down\" or \"none\""); continue; }
                    nc.pull = (uint8_t)p;
                }
                if (db) { if (!json_int(db, 0, IO_DEBOUNCE_MAX, &v)) { snprintf(key, sizeof key, "io.%s.debounce", c->string); err_at(errs, key, "must be an integer 0..1000 (ms)"); continue; } nc.debounce = (uint16_t)v; }
                if (tr) { if (!cJSON_IsBool(tr)) { snprintf(key, sizeof key, "io.%s.trigger", c->string); err_at(errs, key, "must be true or false"); continue; } nc.trigger = cJSON_IsTrue(tr); }
                if (fq) { if (!json_int(fq, IO_FREQ_MIN, IO_FREQ_MAX, &v)) { snprintf(key, sizeof key, "io.%s.freq", c->string); err_at(errs, key, "must be an integer 1..20000 (Hz)"); continue; } nc.freq = (uint16_t)v; }
                if (iv) { if (!cJSON_IsBool(iv)) { snprintf(key, sizeof key, "io.%s.invert", c->string); err_at(errs, key, "must be true or false"); continue; } nc.invert = cJSON_IsTrue(iv); }
                if (nm) {
                    snprintf(key, sizeof key, "io.%s.name", c->string);
                    const char *why = !cJSON_IsString(nm) ? "must be a string" : io_check_name(nm->valuestring);
                    if (why) { err_at(errs, key, why); continue; }
                    memset(nc.name, 0, sizeof nc.name);
                    snprintf(nc.name, sizeof nc.name, "%s", nm->valuestring);
                }
                io[n] = nc;
            }
        } else if (!strcmp(k, "io_set")) {               /* {"0": true, "pump": false, "fan": 0.5}: checked once io is final */
            if (!cJSON_IsObject(item)) err_at(errs, k, "must be an object {\"<n or name>\": true | false | 0..1}");
            else io_set_req = item;
        } else if (!strcmp(k, "save")) {
            if (!cJSON_IsBool(item)) err_at(errs, k, "must be true or false");
            else save = cJSON_IsTrue(item);
        } else if (!strcmp(k, "channels")) {
            if (!cJSON_IsObject(item)) { err_at(errs, k, "must be an object keyed by channel number, e.g. {\"0\": {...}}"); continue; }
            const cJSON *c;
            cJSON_ArrayForEach(c, item) {
                char *end;
                long n = strtol(c->string, &end, 10);
                snprintf(key, sizeof key, "channels.%s", c->string);
                if (!c->string[0] || *end || n < 0 || n >= CHANNELS_MAX) { err_at(errs, key, "unknown channel number"); continue; }
                if (cJSON_IsNull(c)) {                    /* remove: only the last channel, one must remain */
                    if (n != nch - 1 || nch == 1) err_at(errs, key, "only the last channel can be removed, and one must remain");
                    else { nch--; layout_set = true; }
                    continue;
                }
                if (!cJSON_IsObject(c)) { err_at(errs, key, "must be an object {gpio, npix, wire, gamma, dither, wmax, wmode, wpoint, wratio, prog} or null (remove)"); continue; }
                const cJSON *g = cJSON_GetObjectItemCaseSensitive(c, "gpio"), *np = cJSON_GetObjectItemCaseSensitive(c, "npix");
                const cJSON *pr = cJSON_GetObjectItemCaseSensitive(c, "prog"), *ga = cJSON_GetObjectItemCaseSensitive(c, "gamma");
                const cJSON *di = cJSON_GetObjectItemCaseSensitive(c, "dither"), *wi = cJSON_GetObjectItemCaseSensitive(c, "wire");
                const cJSON *wm = cJSON_GetObjectItemCaseSensitive(c, "wmax"), *wo = cJSON_GetObjectItemCaseSensitive(c, "wmode");
                const cJSON *wp = cJSON_GetObjectItemCaseSensitive(c, "wpoint"), *wr = cJSON_GetObjectItemCaseSensitive(c, "wratio");
                const cJSON *ci = cJSON_GetObjectItemCaseSensitive(c, "intensity");
                if (ci) {
                    snprintf(key, sizeof key, "channels.%s.intensity", c->string);
                    if (n >= g_nchannels) err_at(errs, key, "channel isn't running");
                    else if (!cJSON_IsNumber(ci) || ci->valuedouble < 0 || ci->valuedouble > 1) err_at(errs, key, "must be a number 0..1");
                    else cint[n] = ci->valuedouble;
                }
                if (g || np) {
                    if (n > nch) { err_at(errs, key, "channels must be added in order"); continue; }
                    if (n == nch && !(g && np)) { err_at(errs, key, "a new channel needs both gpio and npix"); continue; }
                    long gv = n < nch ? ch[n].gpio : -1, nv = n < nch ? ch[n].npix : 0;
                    snprintf(key, sizeof key, "channels.%s.gpio", c->string);
                    if (g && !json_int(g, 0, 63, &gv)) { err_at(errs, key, "must be an integer"); continue; }
                    const char *why = settings_check_gpio((int)gv);
                    if (why) { err_at(errs, key, why); continue; }
                    snprintf(key, sizeof key, "channels.%s.npix", c->string);
                    if (np && !json_int(np, 1, NPIX_MAX, &nv)) { err_at(errs, key, settings_check_npix(0)); continue; }
                    ch[n].gpio = (int)gv; ch[n].npix = (uint16_t)nv;
                    if (n == nch) { snprintf(ch[n].wire, sizeof ch[n].wire, "%s", WIRE_DEFAULT); nch++; }
                    layout_set = true;
                }
                if (wi) {                                 /* wire format: part of the layout (save + reboot) */
                    snprintf(key, sizeof key, "channels.%s.wire", c->string);
                    wire_t wt;
                    char norm[WIRE_STR_MAX];
                    const char *why = !cJSON_IsString(wi) ? "must be a string, e.g. \"GRB\" or \"BRG WWW\"" : settings_check_wire(wi->valuestring, &wt, norm);
                    if (n >= nch) err_at(errs, key, "no such channel");
                    else if (why) err_at(errs, key, why);
                    else if (strcmp(ch[n].wire, norm)) { snprintf(ch[n].wire, sizeof ch[n].wire, "%s", norm); layout_set = true; }
                }
                if (ga) {                                 /* number 1.0..3.0 (1 = off), rounded to hundredths */
                    snprintf(key, sizeof key, "channels.%s.gamma", c->string);
                    double v = cJSON_IsNumber(ga) ? ga->valuedouble * 100.0 : -1;
                    long gv = (long)(v + 0.5);
                    if (n >= nch) err_at(errs, key, "no such channel");
                    else if (v < GAMMA_MIN - 0.5 || v > GAMMA_MAX + 0.5) err_at(errs, key, "must be a number 1.0..3.0 (1 = off)");
                    else gam[n] = (uint16_t)gv;
                }
                if (di) {
                    long dv;
                    snprintf(key, sizeof key, "channels.%s.dither", c->string);
                    if (n >= nch) err_at(errs, key, "no such channel");
                    else if (!json_int(di, 0, DITHER_MAX, &dv)) err_at(errs, key, "must be an integer 0..4 (bits, 0 = off)");
                    else dith[n] = (uint8_t)dv;
                }
                if (wm) {
                    long v;
                    snprintf(key, sizeof key, "channels.%s.wmax", c->string);
                    if (n >= nch) err_at(errs, key, "no such channel");
                    else if (!json_int(wm, 0, 100, &v)) err_at(errs, key, "must be an integer 0..100 (percent)");
                    else wmx[n] = (uint8_t)v;
                }
                if (wo) {
                    snprintf(key, sizeof key, "channels.%s.wmode", c->string);
                    int mo = cJSON_IsString(wo) ? settings_wmode_parse(wo->valuestring) : -1;
                    if (n >= nch) err_at(errs, key, "no such channel");
                    else if (mo < 0) err_at(errs, key, "must be \"direct\", \"extract\" or \"boost\"");
                    else wmo[n] = (uint8_t)mo;
                }
                if (wp) {
                    snprintf(key, sizeof key, "channels.%s.wpoint", c->string);
                    const char *hx = cJSON_IsString(wp) ? wp->valuestring : "";
                    if (*hx == '#') hx++;
                    char *e2;
                    unsigned long pv = strtoul(hx, &e2, 16);
                    if (n >= nch) err_at(errs, key, "no such channel");
                    else if (strlen(hx) != 6 || *e2 || !pv) err_at(errs, key, "must be a color \"#RRGGBB\" (not black)");
                    else wpt[n] = (uint32_t)pv;
                }
                if (wr) {
                    snprintf(key, sizeof key, "channels.%s.wratio", c->string);
                    double v = cJSON_IsNumber(wr) ? wr->valuedouble * 100.0 : -1;
                    if (n >= nch) err_at(errs, key, "no such channel");
                    else if (v < 9.5 || v > 400.5) err_at(errs, key, "must be a number 0.1..4");
                    else wrt[n] = (uint16_t)(v + 0.5);
                }
                if (pr) {
                    snprintf(key, sizeof key, "channels.%s.prog", c->string);
                    const char *src = cJSON_IsNull(pr) ? "" : cJSON_IsString(pr) ? pr->valuestring : NULL;
                    if (!src) { err_at(errs, key, "must be a string or null"); continue; }
                    if (n >= g_nchannels) { err_at(errs, key, "channel isn't running yet (save + reboot after adding it)"); continue; }
                    if (strlen(src) > SOURCE_MAX) { err_at(errs, key, "too long"); continue; }
                    program_t *p = dsl_compile(&g_channels[n], src, err, sizeof err);
                    if (!p) { err_at(errs, key, err); continue; }
                    program_free(progs[n]);
                    progs[n] = p; prog_src[n] = src;
                }
            }
        } else {
            err_at(errs, k, "unknown field");
        }
    }
    for (int i = 0; i < nch; i++)                         /* final layout: no GPIO twice */
        for (int k2 = i + 1; k2 < nch; k2++)
            if (ch[i].gpio == ch[k2].gpio) { snprintf(key, sizeof key, "channels.%d.gpio", k2); err_at(errs, key, "already used by another channel"); }
    if (io_cfg_set) {                                     /* final outputs: valid, and not on an LED channel's pin */
        int bad;
        const char *why = io_check(io, &bad);
        if (why) { snprintf(key, sizeof key, "io.%d", bad); err_at(errs, key, why); }
        for (int i = 0; i < IO_MAX; i++)
            for (int c2 = 0; c2 < nch && io[i].gpio >= 0; c2++)
                if (io[i].gpio == ch[c2].gpio) { snprintf(key, sizeof key, "io.%d.gpio", i); err_at(errs, key, "that gpio drives an LED channel"); }
    } else if (layout_set) {
        for (int c2 = 0; c2 < nch; c2++)
            if (io_on_gpio(ch[c2].gpio) >= 0) { snprintf(key, sizeof key, "channels.%d.gpio", c2); err_at(errs, key, "that gpio is a GPIO output"); }
    }
    int io_slot[IO_MAX];
    uint16_t io_val[IO_MAX];
    int nio = 0;
    if (io_set_req) {
        const cJSON *e;
        cJSON_ArrayForEach(e, io_set_req) {
            snprintf(key, sizeof key, "io_set.%s", e->string);
            char *end;
            long n = strtol(e->string, &end, 10);
            int slot = -1;
            if (e->string[0] && !*end) slot = n >= 0 && n < IO_MAX && io[n].gpio >= 0 ? (int)n : -1;
            else for (int i = 0; i < IO_MAX; i++) if (io[i].gpio >= 0 && io[i].name[0] && !strcmp(io[i].name, e->string)) slot = i;
            if (slot < 0) { err_at(errs, key, "no such output"); continue; }
            if (!io_mode_is_output(io[slot].mode)) { err_at(errs, key, "that is an input"); continue; }
            long v;
            if (cJSON_IsBool(e)) v = cJSON_IsTrue(e) ? UNIT_ONE : 0;
            else if (cJSON_IsNumber(e) && e->valuedouble >= 0 && e->valuedouble <= 1) v = (long)(e->valuedouble * UNIT_ONE + 0.5);
            else { err_at(errs, key, "must be true, false or a number 0..1"); continue; }
            if (nio < IO_MAX) { io_slot[nio] = slot; io_val[nio++] = (uint16_t)v; }
        }
    }
    /* autoexec last, and only if everything else is fine: running it changes the global buffers */
    if (!cJSON_GetArraySize(errs) && auto_action == 1 && autoexec_run(auto_src, err, sizeof err)) err_at(errs, "autoexec", err);

    if (cJSON_GetArraySize(errs)) {
        cmd_unlock();
        for (int i = 0; i < CHANNELS_MAX; i++) program_free(progs[i]);
        cJSON *j = cJSON_CreateObject();
        cJSON_AddItemToObject(j, "errors", errs);
        return result(400, j);
    }
    cJSON_Delete(errs);

    /* apply */
    g_set.nch = nch;
    memcpy(g_set.ch, ch, sizeof ch);
    for (int i = 0; i < CHANNELS_MAX; i++)
        if (gam[i] != g_set.gamma[i] || dith[i] != g_set.dither[i] || wmx[i] != g_set.wmax[i] || wmo[i] != g_set.wmode[i] ||
            wpt[i] != g_set.wpoint[i] || wrt[i] != g_set.wratio[i]) {
            g_set.gamma[i] = gam[i]; g_set.dither[i] = dith[i]; g_set.wmax[i] = wmx[i];
            g_set.wmode[i] = wmo[i]; g_set.wpoint[i] = wpt[i]; g_set.wratio[i] = wrt[i];
            render_output_update(i);
        }
    uint32_t fade_ms = (uint32_t)(fade_s * 1000 + 0.5);
    if (intensity >= 0) render_set_intensity(INTENSITY_GLOBAL, (uint16_t)intensity, fade_ms);
    for (int i = 0; i < CHANNELS_MAX; i++)
        if (cint[i] >= 0) render_set_intensity(i, (uint16_t)(cint[i] * UNIT_ONE + 0.5), fade_ms);
    if (io_cfg_set) { memcpy(g_set.io, io, sizeof io); io_apply(); }
    for (int i = 0; i < nio; i++) {
        const char *why = io_set(io_slot[i], io_val[i]);
        if (why) ESP_LOGW("api", "io_set %d: %s", io_slot[i], why);
    }
    if (output_set) { g_set.refresh = (uint8_t)refresh; g_set.out_parallel = parallel; render_output_config(); }
    if (tz_set) { strcpy(g_set.tz, tz); settings_apply_tz(); }
    if (ntp_set) strcpy(g_set.ntp, ntp);
    if (loc_set) { if (loc_off) g_set.has_loc = false; else settings_set_loc(loc_lat, loc_lon); }
    if (wifi_set) { strcpy(g_set.ssid, ssid); strcpy(g_set.pass, pass); }
    if (token_set) strcpy(g_set.token, token);
    if (host_set) strcpy(g_set.hostname, host);
#if CONFIG_KTSN_XIAO_C6_ANTENNA
    if (antenna >= 0) { g_set.antenna_ext = antenna == 1; antenna_apply(); }
#endif
#if CONFIG_KTSN_WIFI                                    /* without WiFi they are stored, for a WiFi build to use */
    if (ntp_set) net_ntp_apply();
    if (host_set) net_hostname_apply();
#endif
    if (auto_every > 0) g_set.auto_every = (uint16_t)auto_every;
    if (audio_gate >= 0) g_set.audio_gate = (uint16_t)audio_gate;
    if (automat_action) { char aerr[DSL_ERR_LEN]; automation_set(automat_action == 1 ? automat_src : NULL, aerr, sizeof aerr); }
    if (auto_action == 2) { free(g_set.autoexec); g_set.autoexec = NULL; }
    if (auto_action == 1) { char *copy = strdup(auto_src); if (copy) { free(g_set.autoexec); g_set.autoexec = copy; } }
    for (int i = 0; i < CHANNELS_MAX; i++) if (progs[i]) channel_set_program(&g_channels[i], progs[i], prog_src[i]);
    esp_err_t saved = save ? settings_save() : ESP_OK;
    bool reboot = reboot_needed();
    cmd_unlock();

    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddBoolToObject(j, "saved", save && saved == ESP_OK);
    if (save && saved != ESP_OK) cJSON_AddStringToObject(j, "save_error", esp_err_to_name(saved));
    cJSON_AddBoolToObject(j, "reboot_needed", reboot);
    if (layout_set) cJSON_AddStringToObject(j, "note", "channel layout changes apply after save + reboot");
    api_result_t res = result(200, j);
#if CONFIG_KTSN_WIFI
    if (wifi_set) res.after = apply_wifi_after;
#endif
    return res;
}

/* ---------------- programs, commands, save, reboot ---------------- */

api_result_t api_put_prog(long n, const char *src)
{
    if (strlen(src) > SOURCE_MAX) return api_error(413, "program too long");
    char err[DSL_ERR_LEN];
    cmd_lock();
    if (n < 0 || n >= g_nchannels) { cmd_unlock(); return api_error(404, "no such running channel"); }
    int rc = channel_set_source(&g_channels[n], src, err, sizeof err);
    cmd_unlock();
    return rc ? api_error(400, err) : ok();
}

/* ---------------- program library ---------------- */

static void lib_add(const char *key, size_t len, void *ctx)
{
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "name", key);
    cJSON_AddNumberToObject(e, "size", (double)len);
    cJSON_AddItemToArray((cJSON *)ctx, e);
}

api_result_t api_lib_list(void)
{
    cJSON *j = cJSON_CreateObject(), *arr = cJSON_AddArrayToObject(j, "programs");
    size_t used, total;
    cmd_lock();
    progs_list(PROGS_NS_LIB, lib_add, arr);
    progs_stats(&used, &total);
    cmd_unlock();
    cJSON_AddNumberToObject(j, "used", (double)used);                  /* storage entries (32 bytes each) */
    cJSON_AddNumberToObject(j, "total", (double)total);
    cJSON_AddBoolToObject(j, "separate", progs_separate());            /* false: old layout, programs share the settings NVS */
    return result(200, j);
}

api_result_t api_lib_get(const char *name)
{
    if (lib_check_name(name)) return api_error(404, "no such library program");
    cmd_lock();
    char *src = progs_get(PROGS_NS_LIB, name);
    cmd_unlock();
    if (!src) return api_error(404, "no such library program");
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "name", name);
    cJSON_AddStringToObject(j, "src", src);
    free(src);
    return result(200, j);
}

api_result_t api_lib_put(const char *name, const char *src)
{
    char err[DSL_ERR_LEN];
    cmd_lock();
    const char *why = lib_put(name, src, err, sizeof err);
    cmd_unlock();
    return why ? api_error(strstr(why, "full") ? 413 : 400, why) : ok();
}

api_result_t api_lib_del(const char *name)
{
    if (lib_check_name(name)) return api_error(404, "no such library program");
    char err[DSL_ERR_LEN];
    cmd_lock();
    esp_err_t r = lib_del(name, err, sizeof err);
    cmd_unlock();
    if (r == ESP_ERR_NOT_FOUND) return api_error(404, "no such library program");
    if (r == ESP_ERR_INVALID_STATE) return api_error(409, err);
    return r == ESP_OK ? ok() : api_error(500, esp_err_to_name(r));
}

typedef struct { char *buf; size_t len, cap, limit; bool truncated; } outbuf_t;

static void buf_write(cmd_out_t *o, const char *s, size_t n)
{
    outbuf_t *b = o->ctx;
    if (b->len + n > b->limit) { n = b->len < b->limit ? b->limit - b->len : 0; b->truncated = true; }
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n + 1) cap *= 2;
        char *nb = realloc(b->buf, cap);
        if (!nb) { b->truncated = true; return; }
        b->buf = nb; b->cap = cap;
    }
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = 0;
}

api_result_t api_cmd(const char *text)
{
    if (strlen(text) >= CMD_LINE_MAX) return api_error(413, "command line too long");
    char line[CMD_LINE_MAX];
    strcpy(line, text);
    char *nl = strpbrk(line, "\r\n");
    if (nl) *nl = 0;                                      /* one command per request */
    char *argv0 = line + strspn(line, " \t");
    if (!strncmp(argv0, "reboot", 6) && (!argv0[6] || argv0[6] == ' '))
        return api_error(400, "use the reboot operation");   /* it would never answer */
    outbuf_t ob = { .limit = API_OUTPUT_MAX };
    cmd_out_t o = { .write = buf_write, .ctx = &ob };
    int rc = cmd_exec(&o, line);                          /* takes cmd_lock itself */
    if (ob.truncated) { ob.limit += 32; buf_write(&o, "\n(output truncated)\n", 20); }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "result", rc);
    cJSON_AddStringToObject(j, "output", ob.buf ? ob.buf : "");
    free(ob.buf);
    return result(rc == 0 ? 200 : 400, j);
}

api_result_t api_save(void)
{
    cmd_lock();
    esp_err_t s = settings_save();
    cmd_unlock();
    return s == ESP_OK ? ok() : api_error(500, esp_err_to_name(s));
}

static void restart_after(void)
{
    vTaskDelay(pdMS_TO_TICKS(300));                       /* let the response leave */
    esp_restart();
}

api_result_t api_reboot(void)
{
    api_result_t r = ok();
    r.after = restart_after;
    return r;
}
