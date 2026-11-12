#pragma once
#include <stdint.h>
#include "sdkconfig.h"

#define FRAME_HZ            60
#define GLOBAL_BUF_PIXELS   256      /* length of each of the 8 global buffers */
#define CHANNEL_SCRATCH     2048     /* per-channel arena bytes, reset every frame */
/* Task stacks (bytes) of the tasks that run programs: the evaluator recurses, up to DSL_MAX_EVAL_DEPTH levels with
 * function calls (DSL_MAX_EVAL_STACK is what the compiler allows them). `stat` shows how much was never used. */
#define RENDER_STACK        8192
#define AUTOMATION_STACK    8192
/* Loops in channel programs must leave the render task time to sleep (on single-core chips nothing else runs while it
 * works): all channels share LOOP_LIMIT_US per frame, and each channel gets at least LOOP_MIN_US. */
#define LOOP_LIMIT_US       12000
#define LOOP_MIN_US         2000

#include "wire.h"

/* Channel layout (applied at boot). wire: bytes per pixel on the wire, e.g. "RGB", "GRB", "GRBW", "BRG WWW" (wire.h);
 * set per channel with `chan` (it used to be the compile-time LED_COLOR_FORMAT). */
typedef struct { int gpio; uint16_t npix; char wire[WIRE_STR_MAX]; } led_chan_cfg_t;

#define CHANNELS_MAX        8        /* RMT TX channels on the original ESP32 (C3/C6 have 2; extra channels fail cleanly) */
#define NPIX_MAX            2048     /* per channel; output alone takes ~30 us/px of the 16.6 ms frame */
#define NPIX_MAX_STR        "2048"
#define DEFAULT_NTP_SERVER  "pool.ntp.org"
#define DEFAULT_TZ          "UTC0"   /* POSIX TZ string */
#define DEFAULT_HOSTNAME    "ktsn-led" /* DHCP + mDNS: ktsn-led.local */

/* Default channel layout, used until one is saved with `chan` + `save`. Keep GPIOs off the flash/boot pins. */
#if CONFIG_IDF_TARGET_ESP32
#define DEFAULT_LED_GPIO    25
#elif CONFIG_IDF_TARGET_ESP32C6
#define DEFAULT_LED_GPIO    22       /* XIAO ESP32-C6 D4, matches the user's carrier board; GPIO 24..30 are flash pins */
#else
#define DEFAULT_LED_GPIO    0
#endif
#if CONFIG_IDF_TARGET_ESP32C6 && CONFIG_KTSN_OUTPUT_PARLIO
/* XIAO ESP32-C6 carrier board: two strings on D4 / D5, both driven by one PARLIO transfer */
#define LED_CHANNELS_INIT { \
    { .gpio = 22, .npix = 50, .wire = WIRE_DEFAULT }, \
    { .gpio = 23, .npix = 50, .wire = WIRE_DEFAULT }, \
}
#else
#define LED_CHANNELS_INIT { \
    { .gpio = DEFAULT_LED_GPIO, .npix = 50, .wire = WIRE_DEFAULT }, \
    /* { .gpio = 19, .npix = 100 }, */ \
}
#endif

/* Default GPIO outputs (io.h): slots 0.. in order, used until a layout is saved with `io config` + `save`.
 * Always off at boot. Not defined = no outputs. */
#if CONFIG_IDF_TARGET_ESP32C6
#define IO_DEFAULTS_INIT { { .gpio = 19, .mode = 0, .freq = 1000 }, { .gpio = 20, .mode = 0, .freq = 1000 } }   /* relay drivers */
#endif
