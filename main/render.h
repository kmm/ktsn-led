#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "dsl.h"

typedef struct {
    stat_us_t frame;       /* render ticks: env update + all channels render + output */
    stat_us_t flush;       /* PARLIO: sending all strings at once (RMT sends inside each channel's output) */
    uint32_t  frames;      /* rendered frames (FRAME_HZ) */
    uint32_t  sends;       /* output refreshes (FRAME_HZ x refresh) */
    uint32_t  missed;      /* timer ticks that arrived while the previous one was still running */
    int64_t   t0_us;       /* start of the measurement window */
} render_perf_t;

extern render_perf_t g_perf;

void render_start(void);   /* creates the render task and the periodic frame timer */
void perf_reset(void);
/* Rebuild channel ch's output tables from g_set.gamma / dither / wmax / wmode / wpoint / wratio [ch]; call after changing any (cmd_lock held). */
void render_output_update(int ch);

/* Firmware update progress, 0..1000 (render.c shows it as a bar on every string instead of the programs), -1 = none. */
void render_set_progress(int permille);

/* Bytes of the render task's stack (RENDER_STACK) that were never used since boot. */
uint32_t render_stack_free(void);

/* Intensity, 0..256 (UNIT_ONE = full): global (ch = INTENSITY_GLOBAL, also what programs read as `env intensity`) and
 * per channel; the output uses global x channel. A change can fade: the render loop moves the value a little every
 * frame until it reaches the target (fade_ms 0 = at once). Safe to call from any task. */
#define INTENSITY_GLOBAL (-1)
#define FADE_MAX_MS      3600000u
void     render_set_intensity(int ch, uint16_t target, uint32_t fade_ms);
uint16_t render_intensity(int ch);            /* current value (mid-fade: where it is now) */
uint16_t render_intensity_target(int ch);     /* where it is going (what `save` stores) */

#define REFRESH_MAX 4
/* Apply g_set.refresh (sends per rendered frame, 1..REFRESH_MAX) and g_set.out_parallel (cmd_lock held). */
void render_output_config(void);
/* Would `refresh` sends per frame fit the running strings? NULL if so, else a reason (in why). */
const char *render_check_output(unsigned refresh, bool parallel, char *why, size_t cap);
