#pragma once
#include <stdint.h>

/* Pixel layout: 0xWWRRGGBB. Color order for the wire is applied only in the output stage. */
typedef uint32_t pixel_t;

#define PX_RGB(r, g, b)    ((pixel_t)(((r) & 0xFF) << 16 | ((g) & 0xFF) << 8 | ((b) & 0xFF)))
#define PX_R(p)            (((p) >> 16) & 0xFF)
#define PX_G(p)            (((p) >> 8) & 0xFF)
#define PX_B(p)            ((p) & 0xFF)
#define PX_W(p)            (((p) >> 24) & 0xFF)

#define UNIT_ONE 256u   /* Q8 "1.0": scale factors run 0..256 */

/* Scale all four bytes by s/256 (s in 0..256) with rounding. Two multiplies per pixel:
 * bytes are processed in pairs inside 16-bit lanes, max lane value 255*256+128 < 65536. */
static inline pixel_t px_scale(pixel_t p, uint32_t s)
{
    uint32_t rb = (p & 0x00FF00FFu) * s + 0x00800080u;          /* R,B */
    uint32_t wg = ((p >> 8) & 0x00FF00FFu) * s + 0x00800080u;   /* W,G */
    return ((rb >> 8) & 0x00FF00FFu) | (wg & 0xFF00FF00u);
}

/* Scale all four bytes by s/256, rounded down: what the `scale` native uses. Rounding to nearest would make a fade
 * stick: with s < 256 a byte x stops changing once x*(256-s) < 128 (scale 0.9 holds at 4 forever), so repeated
 * scaling never reaches black. Rounding down always loses at least 1 per step while x > 0 and s < 256. */
static inline pixel_t px_scale_down(pixel_t p, uint32_t s)
{
    uint32_t rb = (p & 0x00FF00FFu) * s;                        /* R,B */
    uint32_t wg = ((p >> 8) & 0x00FF00FFu) * s;                 /* W,G */
    return ((rb >> 8) & 0x00FF00FFu) | (wg & 0xFF00FF00u);
}

/* Map each of the four bytes through a 256-entry table (gamma correction at output). */
static inline pixel_t px_lut(pixel_t p, const uint8_t *t)
{
    return (pixel_t)t[p >> 24] << 24 | (pixel_t)t[(p >> 16) & 0xFF] << 16 | (pixel_t)t[(p >> 8) & 0xFF] << 8 | t[p & 0xFF];
}

/* round(x / 255) for x in 0..65025, no division. */
static inline uint32_t px_div255(uint32_t x)
{
    x += 128;
    return (x + (x >> 8)) >> 8;
}

/* Screen blend per byte: 255 - (255-a)(255-b)/255, rounded. Brightens; black is neutral, white saturates. */
static inline pixel_t px_screen(pixel_t a, pixel_t b)
{
    pixel_t r = 0;
    for (unsigned sh = 0; sh < 32; sh += 8) {
        uint32_t ia = 255u - ((a >> sh) & 0xFFu), ib = 255u - ((b >> sh) & 0xFFu);
        r |= (255u - px_div255(ia * ib)) << sh;
    }
    return r;
}

/* Saturating add per byte: min(255, a + b). */
static inline pixel_t px_add(pixel_t a, pixel_t b)
{
    pixel_t r = 0;
    for (unsigned sh = 0; sh < 32; sh += 8) {
        uint32_t v = ((a >> sh) & 0xFFu) + ((b >> sh) & 0xFFu);
        r |= (v > 255u ? 255u : v) << sh;
    }
    return r;
}

/* Saturating subtract per byte: max(0, a - b). */
static inline pixel_t px_sub(pixel_t a, pixel_t b)
{
    pixel_t r = 0;
    for (unsigned sh = 0; sh < 32; sh += 8) {
        uint32_t x = (a >> sh) & 0xFFu, y = (b >> sh) & 0xFFu;
        r |= (x > y ? x - y : 0u) << sh;
    }
    return r;
}

/* Per-byte maximum ("highest takes precedence"). */
static inline pixel_t px_max(pixel_t a, pixel_t b)
{
    pixel_t r = 0;
    for (unsigned sh = 0; sh < 32; sh += 8) {
        uint32_t x = (a >> sh) & 0xFFu, y = (b >> sh) & 0xFFu;
        r |= (x > y ? x : y) << sh;
    }
    return r;
}

/* Per-byte minimum. */
static inline pixel_t px_min(pixel_t a, pixel_t b)
{
    pixel_t r = 0;
    for (unsigned sh = 0; sh < 32; sh += 8) {
        uint32_t x = (a >> sh) & 0xFFu, y = (b >> sh) & 0xFFu;
        r |= (x < y ? x : y) << sh;
    }
    return r;
}

/* Multiply per byte: a*b/255, rounded. White is neutral, black masks. */
static inline pixel_t px_multiply(pixel_t a, pixel_t b)
{
    pixel_t r = 0;
    for (unsigned sh = 0; sh < 32; sh += 8)
        r |= px_div255(((a >> sh) & 0xFFu) * ((b >> sh) & 0xFFu)) << sh;
    return r;
}

/* Crossfade a -> b by k/256 (k in 0..256), rounded: (a*(256-k) + b*k + 128) / 256.
 * Same two-lane trick as px_scale; each lane stays <= 255*256+128 < 65536. */
static inline pixel_t px_mix(pixel_t a, pixel_t b, uint32_t k)
{
    uint32_t ik = UNIT_ONE - k;
    uint32_t rb = (a & 0x00FF00FFu) * ik + (b & 0x00FF00FFu) * k + 0x00800080u;
    uint32_t wg = ((a >> 8) & 0x00FF00FFu) * ik + ((b >> 8) & 0x00FF00FFu) * k + 0x00800080u;
    return ((rb >> 8) & 0x00FF00FFu) | (wg & 0xFF00FF00u);
}

/* px_mix, but each byte that differs from b moves at least 1 toward it when k > 0: what the buffer `mix` native uses.
 * Plain rounding stops a repeated fade short of its target (|a-b|*k < 128 means no change; mix 0.1 stalls 4 levels
 * away). Never overshoots. One-shot interpolation (gradients, lerp, fractional rotate) keeps px_mix. */
static inline pixel_t px_mix_toward(pixel_t a, pixel_t b, uint32_t k)
{
    pixel_t r = px_mix(a, b, k);
    if (!k || a == b) return r;
    for (unsigned sh = 0; sh < 32; sh += 8) {
        uint32_t x = (a >> sh) & 0xFFu, y = (b >> sh) & 0xFFu;
        if (x != y && ((r >> sh) & 0xFFu) == x) r = (r & ~(0xFFu << sh)) | (y > x ? x + 1 : x - 1) << sh;
    }
    return r;
}
