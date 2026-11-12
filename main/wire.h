#pragma once
#include <stddef.h>
#include <stdint.h>
#include "dither.h"
#include "pixel.h"

/* Wire format of a channel: the bytes sent for one pixel, in order, e.g. "RGB" (WS2811 modules), "GRB" (most WS2812),
 * "GRBW" (SK6812 RGBW), "BRG WWW" (Govee S14: an RGB chip, then a white chip with W on all three outputs).
 * Letters R G B W pick that byte of the pixel (0xWWRRGGBB), 0 sends zero; spaces only group chips for readability.
 * Portable (host tests). */
#define WIRE_MAX      12            /* bytes per pixel */
#define WIRE_STR_MAX  24            /* stored string incl. terminator */
#define WIRE_DEFAULT  "RGB"

typedef struct {
    uint8_t n;                      /* bytes per pixel */
    uint8_t nw;                     /* how many of them are W */
    uint8_t shift[WIRE_MAX];        /* bit position of each byte in the pixel; WIRE_ZERO = always 0 */
} wire_t;

#define WIRE_ZERO 0xFF

/* Parse (case-insensitive) into w and a normalized copy (upper case, single spaces) in norm (WIRE_STR_MAX).
 * NULL if fine, else a reason. */
const char *wire_parse(const char *s, wire_t *w, char *norm);

/* White modes: how a channel with W outputs treats RGB.
 *   DIRECT  W comes only from the program's W byte (default).
 *   EXTRACT the white contained in RGB moves to W: same color, using the white LED.
 *   BOOST   that white is added to W, RGB kept: brighter. */
enum { WMODE_DIRECT = 0, WMODE_EXTRACT, WMODE_BOOST };

/* out_map_t.wf from a white point (0xRRGGBB: what full W looks like) and a ratio (percent: how bright full W is next to
 * that RGB color). Q8 per channel: 256 = a full channel's light. */
static inline void wire_white_factors(uint32_t point, uint32_t ratio100, uint16_t wf[3])
{
    for (int c = 0; c < 3; c++) {
        uint32_t p = (point >> (16 - 8 * c)) & 0xFFu;
        wf[c] = (uint16_t)((p * ratio100 * 256u + 255u * 50u) / (255u * 100u));
    }
}

static inline void wire_encode(const wire_t *w, pixel_t p, uint8_t *out)
{
    for (uint8_t i = 0; i < w->n; i++) out[i] = w->shift[i] == WIRE_ZERO ? 0 : (uint8_t)(p >> w->shift[i]);
}

/* Total white output (0 .. nw x 255) of pixel i: W through the 8.8 gamma table, intensity and the white limit, scaled
 * to nw outputs, then dithered (or rounded) to whole steps. Several W outputs add up (Govee S14: "BRG WWW"), so white
 * has nw x 255 steps: W 255 is still every W output at 255, and the dark end gets nw times finer steps. */
static inline uint32_t wire_white_total(const wire_t *w, const out_map_t *m, pixel_t p, uint32_t i)
{
    uint32_t v = m->gamma16[p >> 24];                                   /* 8.8, 0..65280 */
    uint32_t k = m->intensity16 > OUT_ONE ? OUT_ONE : m->intensity16;
    if (k < OUT_ONE) v = (v * k) >> 16;
    if (m->wmax < UNIT_ONE) v = (v * m->wmax) >> 8;
    v *= w->nw;                                                         /* <= nw x 65280: fits easily */
    return (v + (m->dither ? dither_offset(m->dither, m->frame, i) : 128u)) >> 8;
}

/* A pixel through the output stage (dither.h) and onto the wire. With one W output and no white limit this is exactly
 * out_map_px + wire_encode. Otherwise W comes from wire_white_total, spread over the W outputs in wire order: each gets
 * total / nw, the first (total % nw) one more (e.g. 4 over three: 2 1 1). Needs m->gamma16 (identity: in << 8). */
/* EXTRACT / BOOST, all in linear 8.8 (after the gamma table): the white contained in RGB is the largest amount `a` of
 * W light (m->wf: what full W looks like in RGB) that fits under R, G and B, capped so W stays within the white limit.
 * EXTRACT takes it out of RGB; both add it to W. Then intensity, dither (or rounding) and W spread, as elsewhere. */
static inline void wire_encode_white(const wire_t *w, const out_map_t *m, pixel_t p, uint32_t i, uint8_t *out)
{
    uint32_t lin[3] = { m->gamma16[(p >> 16) & 0xFFu], m->gamma16[(p >> 8) & 0xFFu], m->gamma16[p & 0xFFu] };
    uint32_t wl = m->gamma16[p >> 24];
    uint32_t wmax = m->wmax > UNIT_ONE ? UNIT_ONE : m->wmax, lim = (65280u * wmax) >> 8;
    wl = (wl * wmax) >> 8;
    uint32_t a = 65280u;
    int any = 0;
    for (int c = 0; c < 3; c++)
        if (m->wf[c]) { uint32_t cand = (lin[c] << 8) / m->wf[c]; if (cand < a) a = cand; any = 1; }
    if (!any) a = 0;
    if (a > (lim > wl ? lim - wl : 0)) a = lim > wl ? lim - wl : 0;
    if (m->wmode == WMODE_EXTRACT)
        for (int c = 0; c < 3; c++) { uint32_t take = (a * m->wf[c]) >> 8; lin[c] = lin[c] > take ? lin[c] - take : 0; }
    wl += a;
    uint32_t k = m->intensity16 > OUT_ONE ? OUT_ONE : m->intensity16;
    uint32_t d = m->dither ? dither_offset(m->dither, m->frame, i) : 128u;
    uint8_t rgb[3];
    for (int c = 0; c < 3; c++) { uint32_t v = (k < OUT_ONE ? (lin[c] * k) >> 16 : lin[c]) + d; rgb[c] = (uint8_t)(v > 0xFFFFu ? 255u : v >> 8); }
    uint32_t total = (((k < OUT_ONE ? (wl * k) >> 16 : wl) * w->nw) + d) >> 8, base = total / w->nw, rem = total % w->nw, j = 0;
    for (uint8_t b = 0; b < w->n; b++) {
        uint8_t sh = w->shift[b];
        out[b] = sh == 24 ? (uint8_t)(base + (j++ < rem ? 1u : 0u)) : sh == WIRE_ZERO ? 0 : rgb[(16 - sh) / 8];
    }
}

static inline void wire_encode_mapped(const wire_t *w, const out_map_t *m, pixel_t p, uint32_t i, uint8_t *out)
{
    if (m->wmode != WMODE_DIRECT && w->nw) { wire_encode_white(w, m, p, i, out); return; }
    pixel_t q = out_map_px(m, p, i);
    if (w->nw <= 1 && m->wmax >= UNIT_ONE) { wire_encode(w, q, out); return; }
    uint32_t total = wire_white_total(w, m, p, i), base = w->nw ? total / w->nw : 0, rem = w->nw ? total % w->nw : 0, j = 0;
    for (uint8_t b = 0; b < w->n; b++) {
        if (w->shift[b] == 24) out[b] = (uint8_t)(base + (j++ < rem ? 1u : 0u));
        else out[b] = w->shift[b] == WIRE_ZERO ? 0 : (uint8_t)(q >> w->shift[b]);
    }
}
