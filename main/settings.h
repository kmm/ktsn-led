#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "cmd.h"
#include "config.h"
#include "esp_err.h"
#include "io.h"

/* Everything that `save` persists, except channel programs (those live in g_channels[i].source).
 * Console commands change this in RAM; settings_save() writes it (plus programs) to NVS. */
typedef struct {
    uint8_t        nch;                     /* configured channel layout; applied at boot */
    led_chan_cfg_t ch[CHANNELS_MAX];
    uint16_t       intensity;               /* boot value; the live value is g_env.intensity */
    uint16_t       cintens[CHANNELS_MAX];   /* per channel intensity at boot, 0..256; live: render_intensity(ch) */
    uint16_t       gamma[CHANNELS_MAX];     /* per channel, hundredths (gamma.h): 100 = off; applies at once */
    uint8_t        dither[CHANNELS_MAX];    /* per channel, bits 0..DITHER_MAX (dither.h), 0 = off; applies at once */
    uint8_t        wmax[CHANNELS_MAX];      /* per channel white limit, percent 0..100 (100 = none); applies at once */
    uint8_t        wmode[CHANNELS_MAX];     /* WMODE_* (wire.h): white from RGB; applies at once */
    uint32_t       wpoint[CHANNELS_MAX];    /* 0xRRGGBB: what the W LED looks like (white point) */
    uint16_t       wratio[CHANNELS_MAX];    /* full W = wpoint x wratio/100 in RGB brightness, 10..400 */
    uint8_t        refresh;                 /* output sends per rendered frame, 1..REFRESH_MAX (render.h); at once */
    bool           out_parallel;            /* RMT: send all strings at the same time; at once */
    char           ssid[33], pass[65];      /* empty ssid = WiFi off */
    char           ntp[64], tz[64];
    bool           has_loc;                 /* a location is set: env sunrise, sunset, dawn, dusk */
    int32_t        lat, lon;                /* 1e-5 degrees, north and east positive */
    char           hostname[33];            /* DHCP and mDNS name (<hostname>.local) */
    bool           antenna_ext;             /* XIAO C6 builds (antenna.h): external antenna; applies at once */
    char           token[65];               /* network access token; empty = network CLI/API refuse everyone */
    char          *autoexec;                /* malloc'd source, NULL if none */
    char          *automation;              /* malloc'd source of the automation program, NULL if none (automation.h) */
    uint16_t       auto_every;              /* automation period, seconds 1..3600 */
    io_cfg_t       io[IO_MAX];              /* GPIO outputs (io.h); applies at once, always off at boot */
    uint16_t       audio_gate;              /* audio input noise gate, RMS in ADC counts, 0 = off (audio.h); at once */
} settings_t;

extern settings_t g_set;

#define SOURCE_MAX 3999            /* program source length: NVS strings are limited to 4000 bytes incl. terminator */

esp_err_t settings_init(void);              /* nvs_flash_init + load; defaults for anything missing or invalid */
char     *settings_load_prog(int ch);       /* malloc'd stored program of channel ch, or NULL */
esp_err_t settings_save(void);              /* g_set + running channel programs -> NVS */
void      settings_dump(cmd_out_t *o);      /* print what is stored in NVS (password and token masked) */
esp_err_t settings_erase(void);             /* erase everything stored; defaults apply after reboot */
void      settings_set_loc(double lat, double lon);   /* checked values; applies within a tenth of a second */
void      settings_apply_tz(void);          /* setenv TZ from g_set.tz; affects env hour/day/... immediately */

/* Validation shared by every transport (console, HTTP). NULL if fine, else a short reason. */
const char *settings_check_gpio(int gpio);
const char *settings_check_gpio_in(int gpio);      /* the same for an input pin (input-only pins are fine) */
const char *settings_check_loc(double lat, double lon);
const char *settings_check_npix(long npix);
#define WPOINT_DEFAULT 0xFFB46Bu             /* ~3000 K warm white */
const char *settings_wmode_name(uint8_t mode);       /* "direct" / "extract" / "boost" */
int         settings_wmode_parse(const char *s);     /* WMODE_*, or -1 */
const char *settings_check_wire(const char *s, wire_t *w, char *norm);   /* norm: WIRE_STR_MAX, normalized */
const char *settings_check_wifi(const char *ssid, const char *pass);
const char *settings_check_token(const char *token);
const char *settings_check_hostname(const char *name);
/* Constant-time comparison against g_set.token; false if no token is set. Takes cmd_lock. */
bool        settings_token_ok(const char *given);
