#pragma once
#include <stdint.h>

/* A value 0..256 that can move to a new target over a number of frames (intensity fades). 16.16 fixed point, so even
 * an hour-long fade (216000 frames) moves every frame. Portable (host tests); render.c wraps it in a lock. */
typedef struct { int32_t cur, target, step; } fader_t;

static inline void fader_set(fader_t *f, uint16_t target, int32_t frames)
{
    f->target = (int32_t)target << 16;
    if (frames <= 0) { f->cur = f->target; f->step = 0; return; }
    int32_t d = f->target - f->cur;                      /* magnitude rounded up: arrives within `frames` */
    f->step = d >= 0 ? (d + frames - 1) / frames : -((-d + frames - 1) / frames);
}

static inline void fader_step(fader_t *f)
{
    if (f->cur == f->target) return;
    f->cur += f->step;
    if ((f->step > 0 && f->cur > f->target) || (f->step < 0 && f->cur < f->target) || !f->step) f->cur = f->target;
}

static inline uint16_t fader_value(const fader_t *f)  { return (uint16_t)((f->cur + 32768) >> 16); }
static inline uint16_t fader_target(const fader_t *f) { return (uint16_t)(f->target >> 16); }
/* The value now at output precision, 0..65536 (OUT_ONE): 256 times finer than fader_value, rounded. */
static inline uint32_t fader_value16(const fader_t *f) { return (uint32_t)(f->cur + 128) >> 8; }

/* Global x channel intensity at output precision (both 0..65536), rounded. */
static inline uint32_t intensity16_mul(uint32_t g, uint32_t c) { return (uint32_t)(((uint64_t)g * c + 32768u) >> 16); }
