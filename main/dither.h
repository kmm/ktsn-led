#pragma once
#include <stdint.h>
#include "pixel.h"

/* How a channel's buffer O becomes the bytes sent to its LEDs: gamma, then intensity, then optional ordered temporal
 * dither. Portable (host tests, IDE preview via WebAssembly) and used by both output drivers.
 *
 * Dither keeps `dither` bits (1..DITHER_MAX) below the 8-bit output: the 8.8 value (gamma16, scaled by intensity) gets
 * an offset that cycles through 2^bits evenly spaced steps over 2^bits frames (bit-reversed order, so the on-frames
 * are spread out), so the average over that cycle is the value rounded to 1/2^bits of a step. Each pixel starts the
 * cycle at a different phase (hashed from its index), so pixels don't pulse together. The offset is < 256, so a value
 * of 0 stays 0: a dark channel is never lit. Without gamma or intensity there is nothing to dither (in << 8 + offset
 * >> 8 = in). Bits 0 = off: the plain 8-bit path (gamma table, then px_scale), unchanged. */
#define DITHER_MAX 4

/* Output intensity is 16-bit (OUT_ONE = full), finer than the 0..256 the DSL and settings use, so a slow fade near
 * black moves in small steps (with dither, the bits below the output byte follow it). A whole-Q8 value k8 is exactly
 * k8 << 8 here, and gives the same bytes as the old Q8 path (px_scale rounding). */
#define OUT_ONE 65536u

/* x (one byte) times k / OUT_ONE, rounded to nearest; for k = k8 << 8 this is px_scale's (x * k8 + 128) >> 8. */
static inline pixel_t px_scale16(pixel_t p, uint32_t k)
{
    if (!(k & 0xFFu)) return px_scale(p, k >> 8);       /* steady settings: the old two-lane path, same result */
    pixel_t r = 0;
    for (unsigned sh = 0; sh < 32; sh += 8) r |= ((((p >> sh) & 0xFFu) * k + 32768u) >> 16) << sh;
    return r;
}

typedef struct {
    uint32_t        intensity16; /* 0..OUT_ONE (65536) */
    const uint8_t  *gamma;       /* 8-bit table or NULL (none); used when dither == 0 */
    const uint16_t *gamma16;     /* 8.8 table, required when dither > 0 (identity: in << 8) */
    uint8_t         dither;      /* 0..DITHER_MAX */
    uint32_t        frame;       /* frame counter: advances the dither cycle */
    uint16_t        wmax;        /* white limit, 0..256 (256 = none); used by wire_encode_mapped (wire.h) */
    uint8_t         wmode;       /* WMODE_* (wire.h): white from RGB */
    uint16_t        wf[3];       /* R, G, B light of full W, Q8 (256 = full channel): white point x ratio */
} out_map_t;

/* Offset (0..255, in 1/256 of an output step) for pixel i in this frame; bits 1..DITHER_MAX. */
static inline uint32_t dither_offset(unsigned bits, uint32_t frame, uint32_t i)
{
    uint32_t t = (frame + ((i * 2654435761u) >> (32 - bits))) & ((1u << bits) - 1u), r = 0;
    for (unsigned b = 0; b < bits; b++) r |= ((t >> b) & 1u) << (bits - 1 - b);
    return (2u * r + 1u) << (7 - bits);                  /* step centres: (r + 1/2) / 2^bits */
}

static inline pixel_t out_map_px(const out_map_t *m, pixel_t p, uint32_t i)
{
    uint32_t k = m->intensity16 > OUT_ONE ? OUT_ONE : m->intensity16;
    if (!m->dither) {
        if (m->gamma) p = px_lut(p, m->gamma);
        return k >= OUT_ONE ? p : px_scale16(p, k);
    }
    uint32_t d = dither_offset(m->dither, m->frame, i);
    pixel_t r = 0;
    for (unsigned sh = 0; sh < 32; sh += 8) {
        uint32_t v = m->gamma16[(p >> sh) & 0xFFu];
        if (k < OUT_ONE) v = (v * k) >> 16;               /* v <= 65280, k < 65536: fits 32 bits */
        r |= ((v + d) >> 8) << sh;                        /* v <= 65280, d <= 255: never above 255 */
    }
    return r;
}
