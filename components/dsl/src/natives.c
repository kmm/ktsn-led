#include "dsl_internal.h"
#include <stdio.h>
#include <string.h>

bool dsl_resolve_buf(const channel_t *ch, const char *s, bufref_t *out)
{
    if (!s[0]) return false;
    if (!s[1]) {
        int idx = -1;
        switch (s[0]) {
        case 'X': case 'x': idx = BUF_X; break;
        case 'Y': case 'y': idx = BUF_Y; break;
        case 'Z': case 'z': idx = BUF_Z; break;
        case 'O': case 'o': idx = BUF_O; break;
        }
        if (idx < 0) return false;
        out->px = ch->buf[idx]; out->len = ch->npix; out->stride = 1;
        return true;
    }
    if ((s[0] == 'G' || s[0] == 'g') && s[1] >= '0' && s[1] < '0' + DSL_NUM_GLOBALS && !s[2] && ch->globals) {
        out->px = ch->globals->buf[s[1] - '0']; out->len = ch->globals->npix; out->stride = 1;
        return true;
    }
    return false;
}

/* Optional int arg i, or `def` when omitted or given as `_` (V_NIL). */
static int32_t opt_int(const value_t *a, uint8_t n, uint8_t i, int32_t def)
{
    return (i < n && a[i].type != V_NIL) ? a[i].u.i : def;
}

/* Index into a view of `len` pixels: negative counts from the end, then clamp to [0, len]. */
static int32_t range_index(int32_t i, int32_t len)
{
    if (i < 0) i += len;
    return i < 0 ? 0 : (i > len ? len : i);
}

/* The one range convention for every native: optional [start] [end] at args `first`, `first+1`.
 * Absolute indices, end exclusive; defaults 0 and len (or `_`); negatives count from the end; clamped.
 * Returns false if the range is empty. */
static bool range(const bufref_t *b, const value_t *a, uint8_t n, uint8_t first, int32_t *s, int32_t *e)
{
    *s = range_index(opt_int(a, n, first, 0), b->len);
    *e = range_index(opt_int(a, n, first + 1, b->len), b->len);
    return *s < *e;
}

/* (buf <id>) -> buffer reference. Folded at compile time into a direct pointer. */
static value_t n_buf(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    (void)n;
    value_t v = { .type = V_NIL };
    bufref_t r;
    if (!dsl_resolve_buf(c->ch, a[0].u.sym, &r)) { c->err = "unknown buffer id (use X Y Z O or G0..G7)"; return v; }
    if (c->ch->ctx == DSL_CTX_AUTOMATION && r.px != NULL && (a[0].u.sym[0] | 0x20) != 'g') {
        c->err = "the automation program can only use G0..G7 (it has no X Y Z O)";
        return v;
    }
    v.type = V_BUF; v.u.buf = r;
    return v;
}

/* (slice <buf> [start] [end] [step]) -> view of every step-th pixel of [start, end) (see `range`).
 * step defaults to 1 (or `_`); a negative step selects the same pixels as its positive counterpart, reversed.
 * Pure, so constant slices fold to a pointer at compile time. */
static value_t n_slice(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    const bufref_t *b = &a[0].u.buf;
    int32_t s, e;
    range(b, a, n, 1, &s, &e);
    int32_t step = opt_int(a, n, 3, 1);
    value_t v = { .type = V_NIL };
    if (step == 0) { c->err = "slice step can't be 0"; return v; }
    int32_t k = step < 0 ? -step : step;
    int32_t count = e > s ? (e - s + k - 1) / k : 0;
    v.type = V_BUF;
    v.u.buf.len = (uint16_t)count;
    v.u.buf.stride = b->stride * step;
    if (count == 0) v.u.buf.px = b->px;                    /* empty: never dereferenced */
    else v.u.buf.px = buf_at(b, step > 0 ? s : s + (count - 1) * k);
    return v;
}

/* (fill <buf> <color> [start] [end]) -> buf, range as in `range` */
static value_t n_fill(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    (void)c;
    const bufref_t *b = &a[0].u.buf;
    int32_t s, e;
    if (range(b, a, n, 2, &s, &e)) {
        pixel_t col = a[1].u.color;
        for (ptrdiff_t o = (ptrdiff_t)s * b->stride, end = (ptrdiff_t)e * b->stride; o != end; o += b->stride)
            b->px[o] = col;
    }
    return a[0];
}

/* (scale <buf> <0.0..1.0> [start] [end]) -> buf: multiply brightness (rounded down, so repeated scaling fades to
 * black), range as in `range` */
static value_t n_scale(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    (void)c;
    const bufref_t *b = &a[0].u.buf;
    int32_t s, e;
    uint32_t k = (uint32_t)a[1].u.i;
    if (range(b, a, n, 2, &s, &e) && k != UNIT_ONE)
        for (ptrdiff_t o = (ptrdiff_t)s * b->stride, end = (ptrdiff_t)e * b->stride; o != end; o += b->stride)
            b->px[o] = px_scale_down(b->px[o], k);
    return a[0];
}

/* (invert <buf> [start] [end]) -> buf: every channel x becomes 255 - x (R G B W), range as in `range`.
 * A seg mask (all-on 0xFFFFFFFF / off 0) inverts into the unlit segments. */
static value_t n_invert(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    (void)c;
    const bufref_t *b = &a[0].u.buf;
    int32_t s, e;
    if (range(b, a, n, 1, &s, &e))
        for (ptrdiff_t o = (ptrdiff_t)s * b->stride, end = (ptrdiff_t)e * b->stride; o != end; o += b->stride)
            b->px[o] = ~b->px[o];
    return a[0];
}

/* ---- two-buffer natives: (op <dst> <src> [params...] [start] [end]) -> dst ----
 * dst[i] = op(dst[i], src[i], k) for i in the range, taken over the shorter of the two views.
 * `x` carries the op's own parameters (mix amount, grad-map palette), unused by most. The range args start at index
 * `first`. Overlapping views behave as if src were read completely before any write (like memmove). */
typedef struct { uint32_t k; bufref_t pal; } pair_arg_t;
typedef pixel_t (*pair_fn)(pixel_t dst, pixel_t src, const pair_arg_t *x);

static void view_bounds(const pixel_t *p, ptrdiff_t stride, int32_t cnt, uintptr_t *lo, uintptr_t *hi)
{
    uintptr_t first = (uintptr_t)p, last = (uintptr_t)(p + (ptrdiff_t)(cnt - 1) * stride);
    *lo = first < last ? first : last;
    *hi = first < last ? last : first;
}

/* Copy `cnt` pixels of a view into per-run scratch (contiguous). NULL if scratch is unavailable or full. */
static pixel_t *stage(exec_ctx_t *c, const pixel_t *p, ptrdiff_t stride, int32_t cnt)
{
    pixel_t *tmp = c->scratch ? arena_alloc(c->scratch, (size_t)cnt * sizeof *tmp) : NULL;
    if (tmp) for (int32_t i = 0; i < cnt; i++) tmp[i] = p[(ptrdiff_t)i * stride];
    return tmp;
}

static value_t pairwise(exec_ctx_t *c, const value_t *a, uint8_t n, uint8_t first, pair_fn op, const pair_arg_t *x)
{
    const bufref_t *d = &a[0].u.buf, *s = &a[1].u.buf;
    bufref_t both = { .px = NULL, .len = d->len < s->len ? d->len : s->len, .stride = 1 };
    int32_t st, en;
    if (!range(&both, a, n, first, &st, &en)) return a[0];
    int32_t cnt = en - st;
    pixel_t *dp = buf_at(d, st);
    const pixel_t *sp = buf_at(s, st);
    ptrdiff_t ds = d->stride, ss = s->stride;

    uintptr_t dlo, dhi, slo, shi;
    view_bounds(dp, ds, cnt, &dlo, &dhi);
    view_bounds(sp, ss, cnt, &slo, &shi);
    bool overlap = dlo <= shi && slo <= dhi && !(dp == sp && ds == ss);   /* identical views are safe in place */
    if (overlap && ds == ss) {
        /* Same stride: if dst lies ahead of src in walking direction, walk backwards so src is read first. */
        bool ahead = ds > 0 ? (uintptr_t)dp > (uintptr_t)sp : (uintptr_t)dp < (uintptr_t)sp;
        if (ahead) { dp += (ptrdiff_t)(cnt - 1) * ds; sp += (ptrdiff_t)(cnt - 1) * ss; ds = -ds; ss = -ss; }
    } else if (overlap) {
        /* Different strides (e.g. a buffer reversed onto itself): stage src in per-run scratch. */
        const pixel_t *tmp = stage(c, sp, ss, cnt);
        if (!tmp) { c->err = "not enough scratch memory for overlapping buffers"; return a[0]; }
        sp = tmp; ss = 1;
    }
    for (ptrdiff_t od = 0, os = 0, end = (ptrdiff_t)cnt * ds; od != end; od += ds, os += ss)
        dp[od] = op(dp[od], sp[os], x);
    return a[0];
}

static pixel_t op_copy(pixel_t d, pixel_t s, const pair_arg_t *x)     { (void)d; (void)x; return s; }
static pixel_t op_screen(pixel_t d, pixel_t s, const pair_arg_t *x)   { (void)x; return px_screen(d, s); }
static pixel_t op_add(pixel_t d, pixel_t s, const pair_arg_t *x)      { (void)x; return px_add(d, s); }
static pixel_t op_sub(pixel_t d, pixel_t s, const pair_arg_t *x)      { (void)x; return px_sub(d, s); }
static pixel_t op_lighten(pixel_t d, pixel_t s, const pair_arg_t *x)  { (void)x; return px_max(d, s); }
static pixel_t op_darken(pixel_t d, pixel_t s, const pair_arg_t *x)   { (void)x; return px_min(d, s); }
static pixel_t op_multiply(pixel_t d, pixel_t s, const pair_arg_t *x) { (void)x; return px_multiply(d, s); }
static pixel_t op_mix(pixel_t d, pixel_t s, const pair_arg_t *x)      { return px_mix_toward(d, s, x->k); }

/* (copy <dst> <src> [start] [end]) -> dst */
static value_t n_copy(exec_ctx_t *c, const value_t *a, uint8_t n)     { return pairwise(c, a, n, 2, op_copy, NULL); }
/* (screen <dst> <src> [start] [end]) -> dst */
static value_t n_screen(exec_ctx_t *c, const value_t *a, uint8_t n)   { return pairwise(c, a, n, 2, op_screen, NULL); }
/* (add <dst> <src> [start] [end]) -> dst, saturating */
static value_t n_add(exec_ctx_t *c, const value_t *a, uint8_t n)      { return pairwise(c, a, n, 2, op_add, NULL); }
/* (subtract <dst> <src> [start] [end]) -> dst, saturating at 0 */
static value_t n_subtract(exec_ctx_t *c, const value_t *a, uint8_t n)      { return pairwise(c, a, n, 2, op_sub, NULL); }
/* (lighten <dst> <src> [start] [end]) -> dst: per-channel max ("highest takes precedence") */
static value_t n_lighten(exec_ctx_t *c, const value_t *a, uint8_t n)  { return pairwise(c, a, n, 2, op_lighten, NULL); }
/* (darken <dst> <src> [start] [end]) -> dst: per-channel min */
static value_t n_darken(exec_ctx_t *c, const value_t *a, uint8_t n)   { return pairwise(c, a, n, 2, op_darken, NULL); }
/* (multiply <dst> <src> [start] [end]) -> dst */
static value_t n_multiply(exec_ctx_t *c, const value_t *a, uint8_t n) { return pairwise(c, a, n, 2, op_multiply, NULL); }
/* (mix <dst> <src> <0.0..1.0> [start] [end]) -> dst; 0 keeps dst, 1 is src */
static value_t n_mix(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    pair_arg_t x = { .k = (uint32_t)a[2].u.i };
    if (x.k == 0) return a[0];
    return pairwise(c, a, n, 3, x.k == UNIT_ONE ? op_copy : op_mix, &x);
}

/* Gradient map: src's luma (0..255) picks a position along the palette, first color at 0, last at 255, evenly
 * spaced, linear blend between neighbours. dst's old value is ignored. Luma is BT.601 in 8-bit weights
 * 77/150/29 (sum 256, so white is exactly 255); the W byte is not part of it. */
static pixel_t op_grad_map(pixel_t d, pixel_t s, const pair_arg_t *x)
{
    (void)d;
    uint32_t np = x->pal.len;
    if (np == 1) return *buf_at(&x->pal, 0);
    uint32_t luma = (77u * PX_R(s) + 150u * PX_G(s) + 29u * PX_B(s) + 128u) >> 8;
    uint32_t pos = luma * (np - 1), seg = pos / 255u, rem = pos - seg * 255u;
    pixel_t c0 = *buf_at(&x->pal, (int32_t)seg);
    return rem ? px_mix(c0, *buf_at(&x->pal, (int32_t)seg + 1), (rem * UNIT_ONE + 127u) / 255u) : c0;
}

/* (grad-map <dst> <src> <palette> [start] [end]) -> dst. Empty palette: no-op. A palette that overlaps dst is read
 * from a copy taken before any write (like fill-grad); src may be dst (map in place). */
static value_t n_grad_map(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    pair_arg_t x = { .pal = a[2].u.buf };
    if (x.pal.len == 0) return a[0];
    const bufref_t *d = &a[0].u.buf;
    if (d->len > 0) {
        uintptr_t dlo, dhi, plo, phi;
        view_bounds(d->px, d->stride, d->len, &dlo, &dhi);
        view_bounds(x.pal.px, x.pal.stride, x.pal.len, &plo, &phi);
        if (dlo <= phi && plo <= dhi) {
            pixel_t *tmp = stage(c, x.pal.px, x.pal.stride, x.pal.len);
            if (!tmp) { c->err = "not enough scratch memory for overlapping buffers"; return a[0]; }
            x.pal.px = tmp; x.pal.stride = 1;
        }
    }
    return pairwise(c, a, n, 3, op_grad_map, &x);
}

/* ---- fill variants: (fill-x <buf> <params...> [start] [end]) -> buf ---- */

/* (fill-grad <buf> <palette> [start] [end]): lerp through the palette's colors, spread evenly over the range.
 * First and last palette colors land exactly on the first and last pixel. Empty palette: no-op; one color: fill. */
static value_t n_fill_grad(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    const bufref_t *b = &a[0].u.buf;
    bufref_t pal = a[1].u.buf;
    int32_t s, e;
    if (!range(b, a, n, 2, &s, &e) || pal.len == 0) return a[0];
    int32_t cnt = e - s;
    pixel_t *dp = buf_at(b, s);
    ptrdiff_t ds = b->stride;

    uintptr_t dlo, dhi, plo, phi;                           /* palette inside the range we write: read a copy */
    view_bounds(dp, ds, cnt, &dlo, &dhi);
    view_bounds(pal.px, pal.stride, pal.len, &plo, &phi);
    if (dlo <= phi && plo <= dhi) {
        pixel_t *tmp = stage(c, pal.px, pal.stride, pal.len);
        if (!tmp) { c->err = "not enough scratch memory for overlapping buffers"; return a[0]; }
        pal.px = tmp; pal.stride = 1;
    }

    /* Pixel i sits at palette position i*(np-1)/(cnt-1) = seg + rem/den, stepped without division. */
    uint32_t den = (uint32_t)cnt - 1, step = pal.len - 1u, seg = 0, rem = 0;
    for (ptrdiff_t o = 0, end = (ptrdiff_t)cnt * ds; o != end; o += ds) {
        pixel_t c0 = *buf_at(&pal, (int32_t)seg);
        dp[o] = rem ? px_mix(c0, *buf_at(&pal, (int32_t)seg + 1), (rem * UNIT_ONE + den / 2) / den) : c0;
        for (rem += step; den && rem >= den; rem -= den) seg++;
    }
    return a[0];
}

/* xorshift32, per channel. */
static uint32_t rnd(exec_ctx_t *c)
{
    uint32_t x = c->ch->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return c->ch->rng = x;
}

/* (fill-rand <buf> <color> <min> <max> [start] [end]): each pixel gets color scaled by a random amount in [min, max]. */
static value_t n_fill_rand(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    const bufref_t *b = &a[0].u.buf;
    int32_t s, e;
    if (!range(b, a, n, 4, &s, &e)) return a[0];
    pixel_t col = a[1].u.color;
    uint32_t lo = (uint32_t)a[2].u.i, hi = (uint32_t)a[3].u.i;
    if (lo > hi) { uint32_t t = lo; lo = hi; hi = t; }
    uint32_t span = hi - lo + 1;                            /* 1..257 Q8 steps, both ends reachable */
    for (ptrdiff_t o = (ptrdiff_t)s * b->stride, end = (ptrdiff_t)e * b->stride; o != end; o += b->stride)
        b->px[o] = px_scale(col, lo + (((rnd(c) >> 16) * span) >> 16));
    return a[0];
}

/* (fill-pick <buf> <color1> <color2> <p> [start] [end]): each pixel is color2 with probability p, else color1. */
static value_t n_fill_pick(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    const bufref_t *b = &a[0].u.buf;
    int32_t s, e;
    if (!range(b, a, n, 4, &s, &e)) return a[0];
    pixel_t c1 = a[1].u.color, c2 = a[2].u.color;
    uint32_t p = (uint32_t)a[3].u.i;                        /* Q8: 0 never, 256 always */
    for (ptrdiff_t o = (ptrdiff_t)s * b->stride, end = (ptrdiff_t)e * b->stride; o != end; o += b->stride)
        b->px[o] = (rnd(c) >> 24) < p ? c2 : c1;
    return a[0];
}

/* ---- single pixels: a position clips (out-of-range pixels are skipped), it never clamps ---- */

/* Index for a single position: negative counts from the end once. Result may still be out of range. */
static int32_t pos_index(int32_t i, int32_t len) { return i < 0 ? i + len : i; }

/* (set <buf> <index> <color>...) -> buf: colors go to index, index+1, ...; pixels outside the view are skipped. */
static value_t n_set(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    (void)c;
    const bufref_t *b = &a[0].u.buf;
    int32_t at = pos_index(a[1].u.i, b->len);
    for (uint8_t k = 2; k < n; k++, at++)
        if (at >= 0 && at < b->len) *buf_at(b, at) = a[k].u.color;
    return a[0];
}

/* (get <buf> <index>) -> color; black when the index is outside the view. */
static value_t n_get(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    (void)c; (void)n;
    const bufref_t *b = &a[0].u.buf;
    int32_t at = pos_index(a[1].u.i, b->len);
    value_t v = { .type = V_COLOR };
    v.u.color = (at >= 0 && at < b->len) ? *buf_at(b, at) : 0;
    return v;
}

/* ---- rotate ---- */

/* Reverse pixels [i, j) of a view in place. */
static void reverse(const bufref_t *v, int32_t i, int32_t j)
{
    for (j--; i < j; i++, j--) { pixel_t *p = buf_at(v, i), *q = buf_at(v, j), t = *p; *p = *q; *q = t; }
}

#define ROT_SNAP 655   /* 0.01 in Q16.16: amounts this close to an integer rotate by whole pixels */

/* (rotate <buf> <amount> [start] [end]) -> buf. Rotates the range right (toward higher indices) by amount pixels,
 * left if negative, wrapping around inside the range. In place, no extra buffer:
 * whole pixels by three reversals; a fractional part then blends each pixel with its left neighbour. */
static value_t n_rotate(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    (void)c;
    const bufref_t *b = &a[0].u.buf;
    int32_t s, e;
    if (!range(b, a, n, 2, &s, &e) || e - s < 2) return a[0];
    bufref_t v = { .px = buf_at(b, s), .len = (uint16_t)(e - s), .stride = b->stride };
    int32_t len = v.len, amt = a[1].u.i;                                   /* Q16.16 */

    /* amt = whole + frac/65536 with whole = floor, frac in [0, 65536), without shifting negatives */
    int32_t whole = amt >= 0 ? amt / 65536 : -(int32_t)(((uint32_t)-(int64_t)amt + 65535u) / 65536u);
    int32_t frac = amt - whole * 65536;
    if (frac <= ROT_SNAP) frac = 0;
    else if (frac >= 65536 - ROT_SNAP) { frac = 0; whole++; }

    int32_t k = whole % len;
    if (k < 0) k += len;
    if (k) { reverse(&v, 0, len); reverse(&v, 0, k); reverse(&v, k, len); }

    if (frac) {                       /* shift right by frac: p[i] = mix(p[i], p[i-1], frac), wrapping p[-1] = p[len-1] */
        uint32_t q8 = ((uint32_t)frac + 128u) >> 8;
        pixel_t last = *buf_at(&v, len - 1);
        for (int32_t i = len - 1; i > 0; i--) *buf_at(&v, i) = px_mix(*buf_at(&v, i), *buf_at(&v, i - 1), q8);
        *buf_at(&v, 0) = px_mix(*buf_at(&v, 0), last, q8);
    }
    return a[0];
}

/* ---- environment ---- */
static const char *const k_env_names[] = { "frame", "hour", "minute", "second", "year", "month", "day", "doy", "intensity", "npix",
                                           "sunrise", "sunset", "dawn", "dusk", "moon", "moonlit",
                                           "ms", "weekday", "timeset", "daylight", "daymin", "daysec",
                                           "dayfrac", "hourfrac", "minfrac",
                                           "band0", "band1", "band2", "band3", "band4", "band5", "band6", "band7", "level",
                                           "beats", "beatms" };
enum { ENV_FRAME, ENV_HOUR, ENV_MINUTE, ENV_SECOND, ENV_YEAR, ENV_MONTH, ENV_DAY, ENV_DOY, ENV_INTENSITY, ENV_NPIX,
       ENV_SUNRISE, ENV_SUNSET, ENV_DAWN, ENV_DUSK, ENV_MOON, ENV_MOONLIT,
       ENV_MS, ENV_WEEKDAY, ENV_TIMESET, ENV_DAYLIGHT, ENV_DAYMIN, ENV_DAYSEC,
       ENV_DAYFRAC, ENV_HOURFRAC, ENV_MINFRAC,
       ENV_BAND0, ENV_LEVEL = ENV_BAND0 + DSL_AUDIO_BANDS, ENV_BEATS, ENV_BEATMS, ENV_COUNT };
_Static_assert(sizeof k_env_names / sizeof k_env_names[0] == ENV_COUNT, "k_env_names must list every ENV_*");
/* decimals: the fractions (0.0 <= x < 1.0), the audio bands and level (0..1) */
static bool env_is_fix(int i) { return (i >= ENV_DAYFRAC && i <= ENV_MINFRAC) || (i >= ENV_BAND0 && i <= ENV_LEVEL); }

/* Levenshtein distance of two short names, or `cap` once it reaches it (names longer than 15 count as far). */
static int edit_distance(const char *a, const char *b, int cap)
{
    size_t la = strlen(a), lb = strlen(b);
    if (la > 15 || lb > 15) return cap;
    int row[16];
    for (size_t j = 0; j <= lb; j++) row[j] = (int)j;
    for (size_t i = 1; i <= la; i++) {
        int diag = row[0], low;
        row[0] = low = (int)i;
        for (size_t j = 1; j <= lb; j++) {
            int up = row[j], v = diag + (a[i - 1] != b[j - 1]);
            if (up + 1 < v) v = up + 1;
            if (row[j - 1] + 1 < v) v = row[j - 1] + 1;
            row[j] = v; diag = up;
            if (v < low) low = v;
        }
        if (low >= cap) return cap;
    }
    return row[lb] < cap ? row[lb] : cap;
}

/* Compile time: (env <name>) -> the name becomes an index, so unknown names are compile errors. */
static const char *bind_env(node_t *n, uint8_t *bad)
{
    arg_t *args = n->args;
    *bad = 0;
    if (args[0].node) return "env name must be written directly, e.g. (env second)";
    for (int i = 0; i < ENV_COUNT; i++)
        if (!strcmp(args[0].val.u.sym, k_env_names[i])) {
            args[0].val.type = V_INT; args[0].val.u.i = i;
            n->ret = env_is_fix(i) ? V_FIX : V_INT;
            return NULL;
        }
    /* The names don't fit an error message: suggest the closest one (at most 2 edits), else point at the reference. */
    static char msg[48];                  /* compiles are serialized (cmd_lock on a board, one thread in WASM) */
    int best = 3, bi = -1;
    for (int i = 0; i < ENV_COUNT; i++) {
        int d = edit_distance(args[0].val.u.sym, k_env_names[i], best);
        if (d < best) { best = d; bi = i; }
    }
    if (bi < 0) return "unknown env name (the reference lists them under env)";
    snprintf(msg, sizeof msg, "unknown env name, did you mean %s?", k_env_names[bi]);
    return msg;
}

/* Part `x` of `whole`, as Q16.16 0 <= f < 1 (a leap second's 60 stays below 1). */
static int32_t frac_of(int64_t x, int64_t whole)
{
    int64_t f = x * 65536 / whole;
    return (int32_t)(f > 65535 ? 65535 : f < 0 ? 0 : f);
}

/* (env <name>) -> int (or decimal for the fractions), read fresh every frame */
static value_t n_env(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    (void)n;
    const env_t *e = c->env;
    value_t v = { .type = V_INT };
    int64_t sec = (int64_t)e->hour * 3600 + e->minute * 60 + e->second;     /* by the clock, like hour and minute */
    switch (a[0].u.i) {
    case ENV_FRAME:     v.u.i = (int32_t)(e->frame & 0x7FFFFFFFu); break;   /* never negative: negatives index from the end */
    case ENV_HOUR:      v.u.i = e->hour; break;
    case ENV_MINUTE:    v.u.i = e->minute; break;
    case ENV_SECOND:    v.u.i = e->second; break;
    case ENV_YEAR:      v.u.i = e->year; break;
    case ENV_MONTH:     v.u.i = e->month; break;
    case ENV_DAY:       v.u.i = e->day; break;
    case ENV_DOY:       v.u.i = e->doy; break;
    case ENV_INTENSITY: v.u.i = e->intensity; break;
    case ENV_NPIX:      v.u.i = c->ch ? c->ch->npix : 0; break;    /* the channel this program runs on */
    case ENV_SUNRISE:   v.u.i = e->sunrise; break;
    case ENV_SUNSET:    v.u.i = e->sunset; break;
    case ENV_DAWN:      v.u.i = e->dawn; break;
    case ENV_DUSK:      v.u.i = e->dusk; break;
    case ENV_MOON:      v.u.i = e->moon; break;
    case ENV_MOONLIT:   v.u.i = e->moonlit; break;
    case ENV_MS:        v.u.i = e->ms; break;
    case ENV_WEEKDAY:   v.u.i = e->weekday; break;
    case ENV_TIMESET:   v.u.i = e->timeset; break;
    case ENV_DAYLIGHT:  v.u.i = e->daylight; break;
    case ENV_DAYMIN:    v.u.i = e->hour * 60 + e->minute; break;
    case ENV_DAYSEC:    v.u.i = (int32_t)sec; break;
    case ENV_DAYFRAC:   v.type = V_FIX; v.u.i = frac_of(sec * 1000 + e->ms, 86400000); break;
    case ENV_HOURFRAC:  v.type = V_FIX; v.u.i = frac_of((sec % 3600) * 1000 + e->ms, 3600000); break;
    case ENV_MINFRAC:   v.type = V_FIX; v.u.i = frac_of((int64_t)e->second * 1000 + e->ms, 60000); break;
    case ENV_LEVEL:     v.type = V_FIX; v.u.i = (int32_t)(e->level > 65536 ? 65536 : e->level); break;
    case ENV_BEATS:     v.u.i = (int32_t)(e->beats & 0x7FFFFFFFu); break;
    case ENV_BEATMS:    v.u.i = e->beatms; break;
    default:
        if (a[0].u.i >= ENV_BAND0 && a[0].u.i < ENV_LEVEL) {
            uint32_t b = e->band[a[0].u.i - ENV_BAND0];
            v.type = V_FIX; v.u.i = (int32_t)(b > 65536 ? 65536 : b);
        }
        break;
    }
    return v;
}

/* ---- logic: booleans are their own type (t / nil); numbers must be compared explicitly ---- */

static value_t boolv(bool b) { value_t v = { .type = V_BOOL }; v.u.i = b; return v; }
static bool numeric(uint8_t t) { return t == V_INT || t == V_FIX; }
static uint8_t arg_type(const arg_t *a) { return a->node ? a->node->ret : a->val.type; }

/* Compare two numbers (int or Q16.16) without losing range: ints compare directly, mixed ones in 64-bit Q16.16. */
static int cmp_num(value_t a, value_t b)
{
    if (a.type == V_INT && b.type == V_INT) return (a.u.i > b.u.i) - (a.u.i < b.u.i);
    int64_t x = a.type == V_INT ? (int64_t)a.u.i * 65536 : a.u.i;
    int64_t y = b.type == V_INT ? (int64_t)b.u.i * 65536 : b.u.i;
    return (x > y) - (x < y);
}

static value_t n_lt(exec_ctx_t *c, const value_t *a, uint8_t n) { (void)c; (void)n; return boolv(cmp_num(a[0], a[1]) < 0); }
static value_t n_gt(exec_ctx_t *c, const value_t *a, uint8_t n) { (void)c; (void)n; return boolv(cmp_num(a[0], a[1]) > 0); }
static value_t n_le(exec_ctx_t *c, const value_t *a, uint8_t n) { (void)c; (void)n; return boolv(cmp_num(a[0], a[1]) <= 0); }
static value_t n_ge(exec_ctx_t *c, const value_t *a, uint8_t n) { (void)c; (void)n; return boolv(cmp_num(a[0], a[1]) >= 0); }

static bool values_equal(value_t a, value_t b)
{
    if (numeric(a.type) && numeric(b.type)) return cmp_num(a, b) == 0;
    switch (a.type) {
    case V_COLOR: return a.u.color == b.u.color;
    case V_BOOL:  return a.u.i == b.u.i;
    case V_BUF:   return a.u.buf.px == b.u.buf.px && a.u.buf.len == b.u.buf.len && a.u.buf.stride == b.u.buf.stride;
    case V_STR:   return b.type == V_STR && a.u.str.len == b.u.str.len && !memcmp(a.u.str.p, b.u.str.p, a.u.str.len);
    default:      return false;
    }
}

/* eq/ne: two numbers (int and decimal mix), or two colors, booleans, buffers (same view) or strings (same text). */
static const char *bind_eq(node_t *n, uint8_t *bad)
{
    uint8_t ta = arg_type(&n->args[0]), tb = arg_type(&n->args[1]);
    if (numeric(ta) && numeric(tb)) return NULL;
    if (ta == tb && (ta == V_COLOR || ta == V_BOOL || ta == V_BUF || ta == V_STR)) return NULL;
    *bad = numeric(ta) || ta == V_COLOR || ta == V_BOOL || ta == V_BUF || ta == V_STR ? 1 : 0;
    return "can only compare two numbers, colors, booleans, buffers or strings";
}
static value_t n_eq(exec_ctx_t *c, const value_t *a, uint8_t n) { (void)c; (void)n; return boolv(values_equal(a[0], a[1])); }
static value_t n_ne(exec_ctx_t *c, const value_t *a, uint8_t n) { (void)c; (void)n; return boolv(!values_equal(a[0], a[1])); }
static value_t n_not(exec_ctx_t *c, const value_t *a, uint8_t n) { (void)c; (void)n; return boolv(!a[0].u.i); }

/* (and a b ...) / (or a b ...): short-circuit, left to right. */
static value_t n_and(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    (void)a;
    const node_t *self = c->node;
    for (uint8_t i = 0; i < n; i++) {
        value_t v = dsl_eval_arg(c, self, i);
        if (c->err || !v.u.i) return boolv(false);
    }
    return boolv(true);
}
static value_t n_or(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    (void)a;
    const node_t *self = c->node;
    for (uint8_t i = 0; i < n; i++) {
        value_t v = dsl_eval_arg(c, self, i);
        if (c->err) return boolv(false);
        if (v.u.i) return boolv(true);
    }
    return boolv(false);
}

/* (if <cond> <then> [else]): result type is the branches' common type (int + decimal -> decimal);
 * without an else, or if a branch returns nothing, `if` is a statement and returns nothing. A self tail call branch
 * (V_TAILCALL) takes the other branch's type: that is what the function returns. */
static const char *bind_if(node_t *n, uint8_t *bad)
{
    uint8_t t = arg_type(&n->args[1]), f = n->nargs > 2 ? arg_type(&n->args[2]) : V_NIL;
    if (t == V_SYM || f == V_SYM) { *bad = t == V_SYM ? 1 : 2; return "'if' branches can't be bare names"; }
    if (t == V_TAILCALL) t = f;
    if (f == V_TAILCALL) f = t;
    if (t == V_NIL || f == V_NIL) n->ret = V_NIL;
    else if (t == f) n->ret = t;
    else if (numeric(t) && numeric(f)) n->ret = V_FIX;
    else { *bad = 2; return "'if' branches must have the same type"; }
    return NULL;
}
static value_t n_if(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    (void)a;
    const node_t *self = c->node;
    value_t nil = { .type = V_NIL };
    value_t cond = dsl_eval_arg(c, self, 0);
    if (c->err) return nil;
    uint8_t pick = cond.u.i ? 1 : 2;
    if (pick >= n) return nil;
    value_t r = dsl_eval_arg(c, self, pick);
    if (c->err || self->ret == V_NIL) return nil;
    if (self->ret == V_FIX) dsl_coerce(A_NUM, r, &r);     /* int branch of a mixed int/decimal if */
    return r;
}

/* ---- arithmetic: ints stay ints, any decimal makes a decimal (Q16.16). 64-bit intermediates, saturating results ---- */

static int64_t q16(value_t v) { return v.type == V_FIX ? v.u.i : (int64_t)v.u.i * 65536; }
static value_t sat(int64_t x, uint8_t type)
{
    value_t v = { .type = type };
    v.u.i = x > INT32_MAX ? INT32_MAX : x < -INT32_MAX ? -INT32_MAX : (int32_t)x;
    return v;
}
static int64_t floordiv64(int64_t a, int64_t b) { int64_t q = a / b; return (a % b && (a < 0) != (b < 0)) ? q - 1 : q; }
static int64_t floormod64(int64_t a, int64_t b) { int64_t r = a % b; return (r && (r < 0) != (b < 0)) ? r + b : r; }
static bool any_fix(const value_t *a, uint8_t n) { for (uint8_t i = 0; i < n; i++) if (a[i].type == V_FIX) return true; return false; }

/* Result type is int unless an argument is a decimal. */
static const char *bind_arith(node_t *n, uint8_t *bad)
{
    (void)bad;
    n->ret = V_INT;
    for (uint8_t i = 0; i < n->nargs; i++) if (arg_type(&n->args[i]) == V_FIX) n->ret = V_FIX;
    return NULL;
}

static value_t n_plus(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    (void)c;
    bool fix = any_fix(a, n);
    int64_t acc = 0;
    for (uint8_t i = 0; i < n; i++) acc += fix ? q16(a[i]) : a[i].u.i;
    return sat(acc, fix ? V_FIX : V_INT);
}

/* (- x) negates, (- a b) subtracts */
static value_t n_minus(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    (void)c;
    bool fix = any_fix(a, n);
    int64_t x = fix ? q16(a[0]) : a[0].u.i;
    if (n == 1) return sat(-x, fix ? V_FIX : V_INT);
    return sat(x - (fix ? q16(a[1]) : a[1].u.i), fix ? V_FIX : V_INT);
}

/* Left to right; each step multiplies in whichever form is exact (int*int, int*decimal, decimal*decimal), so a big
 * int scaled by a small decimal, e.g. (* 100000 0.001), doesn't saturate first. Saturating each step keeps 64 bits enough. */
static value_t n_times(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    (void)c;
    value_t acc = a[0];
    for (uint8_t i = 1; i < n; i++) {
        int64_t p = (int64_t)acc.u.i * a[i].u.i;
        if (acc.type == V_FIX && a[i].type == V_FIX) acc = sat(p / 65536, V_FIX);
        else acc = sat(p, acc.type == V_FIX || a[i].type == V_FIX ? V_FIX : V_INT);
    }
    return acc;
}

/* (/ a b): two ints -> int, rounded down (floor); otherwise decimal. Dividing by zero is a fault. */
static value_t n_div(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    value_t nil = { .type = V_NIL };
    if (a[1].u.i == 0) { c->err = "division by zero"; return nil; }
    if (!any_fix(a, n)) return sat(floordiv64(a[0].u.i, a[1].u.i), V_INT);
    int64_t num = q16(a[0]), den = q16(a[1]);
    int64_t q = num / den, r = num % den;                  /* Q16 / Q16 -> Q16 by long division, no 128-bit math */
    if (q > 32767 || q < -32767) return sat(q > 0 ? INT32_MAX : -INT32_MAX, V_FIX);
    return sat(q * 65536 + r * 65536 / den, V_FIX);
}

/* (mod a b): result has the sign of b (floor modulo), so (mod -1 60) is 59. Works for ints and decimals. */
static value_t n_mod(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    value_t nil = { .type = V_NIL };
    if (a[1].u.i == 0) { c->err = "division by zero"; return nil; }
    if (!any_fix(a, n)) return sat(floormod64(a[0].u.i, a[1].u.i), V_INT);
    return sat(floormod64(q16(a[0]), q16(a[1])), V_FIX);
}

static value_t n_abs(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    (void)c; (void)n;
    return sat(a[0].u.i < 0 ? -(int64_t)a[0].u.i : a[0].u.i, a[0].type);
}

/* (min a b ...) / (max a b ...) */
static value_t minmax(const value_t *a, uint8_t n, bool want_max)
{
    bool fix = any_fix(a, n);
    int64_t best = fix ? q16(a[0]) : a[0].u.i;
    for (uint8_t i = 1; i < n; i++) {
        int64_t x = fix ? q16(a[i]) : a[i].u.i;
        if (want_max ? x > best : x < best) best = x;
    }
    return sat(best, fix ? V_FIX : V_INT);
}
static value_t n_min(exec_ctx_t *c, const value_t *a, uint8_t n) { (void)c; return minmax(a, n, false); }
static value_t n_max(exec_ctx_t *c, const value_t *a, uint8_t n) { (void)c; return minmax(a, n, true); }

/* (clamp x lo hi) */
static value_t n_clamp(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    (void)c;
    bool fix = any_fix(a, n);
    int64_t x = fix ? q16(a[0]) : a[0].u.i, lo = fix ? q16(a[1]) : a[1].u.i, hi = fix ? q16(a[2]) : a[2].u.i;
    if (lo > hi) { int64_t t = lo; lo = hi; hi = t; }
    return sat(x < lo ? lo : x > hi ? hi : x, fix ? V_FIX : V_INT);
}

/* decimal -> int */
static value_t n_floor(exec_ctx_t *c, const value_t *a, uint8_t n) { (void)c; (void)n; return sat(floordiv64(q16(a[0]), 65536), V_INT); }
static value_t n_round(exec_ctx_t *c, const value_t *a, uint8_t n) { (void)c; (void)n; return sat(floordiv64(q16(a[0]) + 32768, 65536), V_INT); }

/* sin(0..90 degrees) in Q16.16, 1 degree steps. Other quadrants by symmetry; linear interpolation between degrees. */
static const int32_t k_sin_q16[91] = {
    0, 1144, 2287, 3430, 4572, 5712, 6850, 7987, 9121, 10252,
    11380, 12505, 13626, 14742, 15855, 16962, 18064, 19161, 20252, 21336,
    22415, 23486, 24550, 25607, 26656, 27697, 28729, 29753, 30767, 31772,
    32768, 33754, 34729, 35693, 36647, 37590, 38521, 39441, 40348, 41243,
    42126, 42995, 43852, 44695, 45525, 46341, 47143, 47930, 48703, 49461,
    50203, 50931, 51643, 52339, 53020, 53684, 54332, 54963, 55578, 56175,
    56756, 57319, 57865, 58393, 58903, 59396, 59870, 60326, 60764, 61183,
    61584, 61966, 62328, 62672, 62997, 63303, 63589, 63856, 64104, 64332,
    64540, 64729, 64898, 65048, 65177, 65287, 65376, 65446, 65496, 65526,
    65536,
};

static int32_t sin_deg(int32_t d)   /* d in 0..359 */
{
    if (d <= 90)  return k_sin_q16[d];
    if (d <= 180) return k_sin_q16[180 - d];
    if (d <= 270) return -k_sin_q16[d - 180];
    return -k_sin_q16[360 - d];
}

/* Sine of an angle in degrees given as Q16.16 (any range; ints are passed in exactly, so huge counts still wrap). */
static value_t sin_q16(int64_t deg_q16)
{
    int64_t q = floormod64(deg_q16, (int64_t)360 * 65536);
    int32_t d = (int32_t)(q >> 16), frac = (int32_t)(q & 0xFFFF);
    int32_t lo = sin_deg(d), hi = sin_deg(d == 359 ? 0 : d + 1);
    return sat(lo + (int64_t)(hi - lo) * frac / 65536, V_FIX);
}
/* Reduce ints mod 360 first so q16() can't overflow on huge values like (env frame). */
static int64_t angle_q16(value_t v) { return v.type == V_INT ? floormod64(v.u.i, 360) * 65536 : v.u.i; }

static value_t n_sin(exec_ctx_t *c, const value_t *a, uint8_t n) { (void)c; (void)n; return sin_q16(angle_q16(a[0])); }
static value_t n_cos(exec_ctx_t *c, const value_t *a, uint8_t n) { (void)c; (void)n; return sin_q16(angle_q16(a[0]) + 90 * 65536); }

/* (tri deg): triangle wave with sin's period and phase: 0 at 0, 1 at 90, 0 at 180, -1 at 270; straight lines between. */
static value_t n_tri(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    (void)c; (void)n;
    int64_t q = floormod64(angle_q16(a[0]), (int64_t)360 * 65536), d90 = (int64_t)90 * 65536;
    int64_t x = q < d90 ? q : q < 3 * d90 ? 2 * d90 - q : q - 4 * d90;    /* -90..90 degrees, Q16 */
    return sat(x * 65536 / d90, V_FIX);
}

/* (lerp a b t): numbers -> a + (b - a) * t, t not clamped (extrapolates); int result only if all three are ints.
 * Two colors -> the color t of the way from a to b, t clamped to 0..1 (like mix). */
static const char *bind_lerp(node_t *n, uint8_t *bad)
{
    uint8_t ta = arg_type(&n->args[0]), tb = arg_type(&n->args[1]);
    if (ta == V_COLOR && tb == V_COLOR) { n->ret = V_COLOR; return NULL; }
    if (numeric(ta) && numeric(tb)) return bind_arith(n, bad);
    *bad = numeric(ta) || ta == V_COLOR ? 1 : 0;
    return "lerp needs two numbers or two colors";
}
static value_t n_lerp(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    (void)c; (void)n;
    if (a[0].type == V_COLOR) {
        int64_t t = q16(a[2]);
        uint32_t k = t <= 0 ? 0 : t >= 65536 ? UNIT_ONE : (uint32_t)((t * UNIT_ONE + 32768) >> 16);
        value_t v = { .type = V_COLOR };
        v.u.color = px_mix(a[0].u.color, a[1].u.color, k);
        return v;
    }
    if (!any_fix(a, 3)) return sat(a[0].u.i + ((int64_t)a[1].u.i - a[0].u.i) * a[2].u.i, V_INT);
    int64_t x = q16(a[0]), y = q16(a[1]), t = q16(a[2]);
    int64_t d = y - x, whole = t >> 16, frac = t & 0xFFFF;  /* |d| < 2^49: split both so nothing overflows 64 bits */
    if (d && (whole > INT64_MAX / 4 / (d < 0 ? -d : d) || whole < -INT64_MAX / 4 / (d < 0 ? -d : d)))
        return sat((d < 0) != (whole < 0) ? -INT64_MAX : INT64_MAX, V_FIX);
    return sat(x + d * whole + (d >> 16) * frac + (((d & 0xFFFF) * frac) >> 16), V_FIX);
}

/* (rand) -> decimal in [0, 1); (rand hi) -> [0, hi); (rand lo hi) -> [lo, hi). Limits in either order: always [min, max).
 * Ints give ints, any decimal gives a decimal. New value every call, from the channel's random sequence. */
static const char *bind_rand(node_t *n, uint8_t *bad)
{
    for (uint8_t i = 0; i < n->nargs; i++)
        if (arg_type(&n->args[i]) == V_NIL) { *bad = i; return "rand has no default to skip: use (rand), (rand hi) or (rand lo hi)"; }
    if (n->nargs == 0) { n->ret = V_FIX; return NULL; }
    return bind_arith(n, bad);
}
static value_t n_rand(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    bool fix = n == 0 || any_fix(a, n);
    int64_t lo = 0, hi = 65536;
    if (n == 1) hi = fix ? q16(a[0]) : a[0].u.i;
    if (n == 2) { lo = fix ? q16(a[0]) : a[0].u.i; hi = fix ? q16(a[1]) : a[1].u.i; }
    if (lo > hi) { int64_t t = lo; lo = hi; hi = t; }
    uint64_t span = (uint64_t)(hi - lo);                    /* < 2^33 */
    return sat(lo + (int64_t)(((uint64_t)rnd(c) * span) >> 32), fix ? V_FIX : V_INT);
}

/* ---- strings: views of text (literals in the program, results in the per-run scratch) ---- */

static char *str_alloc(exec_ctx_t *c, size_t n)
{
    char *p = c->scratch ? arena_alloc(c->scratch, n ? n : 1) : NULL;
    if (!p) c->err = "not enough scratch memory for strings";
    return p;
}

static value_t strv(const char *p, size_t len) { value_t v = { .type = V_STR }; v.u.str.p = p; v.u.str.len = (uint16_t)len; return v; }

#define STRNUM_WIDTH_MAX 32
#define STRNUM_DEC_MAX   4

/* A number as text: `dec` digits after the point (decimals are rounded, half away from zero), at least `width`
 * characters, padded on the left with blanks (or with zeros after the sign). out holds 48. Returns the length. */
static int fmt_num(char *out, value_t v, int width, int dec, bool zero)
{
    static const uint32_t k_pow10[] = { 1, 10, 100, 1000, 10000 };
    bool neg = v.u.i < 0;
    uint64_t mag = neg ? (uint64_t)(-(int64_t)v.u.i) : (uint64_t)v.u.i, ip, fp = 0;
    uint32_t scale = k_pow10[dec];
    if (v.type == V_FIX) {
        uint64_t r = (mag * scale + 32768u) >> 16;
        ip = r / scale; fp = r % scale;
    } else ip = mag;
    if (!ip && !fp) neg = false;                          /* no "-0" */
    char body[24];
    int n = snprintf(body, sizeof body, "%llu", (unsigned long long)ip);
    if (dec) { body[n++] = '.'; for (int k = dec - 1; k >= 0; k--) body[n + k] = (char)('0' + fp % 10), fp /= 10; n += dec; }
    int len = n + neg, pad = width > len ? width - len : 0, o = 0;
    if (!zero) while (pad--) out[o++] = ' ';
    if (neg) out[o++] = '-';
    if (zero) while (pad-- > 0) out[o++] = '0';
    memcpy(out + o, body, (size_t)n);
    return o + n;
}

static int strnum_dec(const value_t *a, uint8_t n, uint8_t i, uint8_t type)
{
    int32_t d = opt_int(a, n, i, type == V_FIX ? 2 : 0);
    return d < 0 ? 0 : d > STRNUM_DEC_MAX ? STRNUM_DEC_MAX : d;
}

/* (str-num <number> [width] [decimals] [zero]) -> string: ints as they are, decimals with 2 digits after the point
 * unless decimals says otherwise; right-aligned to width with blanks (zero: with zeros). */
static value_t n_str_num(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    int32_t w = opt_int(a, n, 1, 0);
    char buf[48];
    int len = fmt_num(buf, a[0], w < 0 ? 0 : w > STRNUM_WIDTH_MAX ? STRNUM_WIDTH_MAX : w, strnum_dec(a, n, 2, a[0].type),
                      n > 3 && a[3].type == V_BOOL && a[3].u.i);
    char *p = str_alloc(c, (size_t)len);
    if (!p) return strv("", 0);
    memcpy(p, buf, (size_t)len);
    return strv(p, (size_t)len);
}

static const char *bind_str_num(node_t *n, uint8_t *bad)
{
    if (n->nargs > 1 && !n->args[1].node && n->args[1].val.type == V_INT && (n->args[1].val.u.i < 0 || n->args[1].val.u.i > STRNUM_WIDTH_MAX)) {
        *bad = 1; return "width must be 0..32";
    }
    if (n->nargs > 2 && !n->args[2].node && n->args[2].val.type == V_INT && (n->args[2].val.u.i < 0 || n->args[2].val.u.i > STRNUM_DEC_MAX)) {
        *bad = 2; return "decimals must be 0..4";
    }
    return NULL;
}

/* str-time codes: H k I l M S p Y y m d e j %. NULL if fine, else what is wrong. */
static const char *time_fmt_check(strref_t f)
{
    for (uint16_t i = 0; i < f.len; i++) {
        if (f.p[i] != '%') continue;
        if (++i == f.len) return "str-time: '%' at the end (write %% for a percent sign)";
        if (!strchr("HkIlMSpYymdej%", f.p[i]) || !f.p[i]) return "str-time: unknown %-code (H k I l M S p Y y m d e j %)";
    }
    return NULL;
}

/* (str-time <format>) -> string: the clock (env) through a strftime-like format, e.g. "%H:%M". */
static value_t n_str_time(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    (void)n;
    strref_t f = a[0].u.str;
    if ((c->err = time_fmt_check(f))) return strv("", 0);
    size_t cap = (size_t)f.len * 2 + 1;                   /* every code is 2 characters and becomes at most 4 */
    char *p = str_alloc(c, cap);
    if (!p) return strv("", 0);
    const env_t *e = c->env;
    size_t o = 0;
    for (uint16_t i = 0; i < f.len; i++) {
        if (f.p[i] != '%') { p[o++] = f.p[i]; continue; }
        int h12 = e->hour % 12 ? e->hour % 12 : 12;
        char t[8];
        switch (f.p[++i]) {
        case 'H': snprintf(t, sizeof t, "%02d", (int)e->hour); break;
        case 'k': snprintf(t, sizeof t, "%2d", (int)e->hour); break;
        case 'I': snprintf(t, sizeof t, "%02d", h12); break;
        case 'l': snprintf(t, sizeof t, "%2d", h12); break;
        case 'M': snprintf(t, sizeof t, "%02d", (int)e->minute); break;
        case 'S': snprintf(t, sizeof t, "%02d", (int)e->second); break;
        case 'p': snprintf(t, sizeof t, "%s", e->hour < 12 ? "AM" : "PM"); break;
        case 'Y': snprintf(t, sizeof t, "%04d", (int)(e->year % 10000u)); break;
        case 'y': snprintf(t, sizeof t, "%02d", (int)(e->year % 100u)); break;
        case 'm': snprintf(t, sizeof t, "%02d", (int)e->month); break;
        case 'd': snprintf(t, sizeof t, "%02d", (int)e->day); break;
        case 'e': snprintf(t, sizeof t, "%2d", (int)e->day); break;
        case 'j': snprintf(t, sizeof t, "%03d", (int)e->doy); break;
        default:  snprintf(t, sizeof t, "%%"); break;
        }
        for (const char *q = t; *q && o < cap; q++) p[o++] = *q;
    }
    return strv(p, o > DSL_STR_MAX ? DSL_STR_MAX : o);
}

static const char *bind_str_time(node_t *n, uint8_t *bad)
{
    *bad = 0;
    return !n->args[0].node ? time_fmt_check(n->args[0].val.u.str) : NULL;
}

/* ---- date and time comparisons (time-gt ... time-in, date-gt ... date-in) ----
 * A time is a "HH:MM" / "HH:MM:SS" string or a number of minutes after midnight (int: to the minute, decimal: to the
 * second; wraps at 24 h; negative, e.g. (env sunset) without a location, makes the test nil). The comparisons work at
 * the precision the time is given in, so (time-eq "18:30") holds all minute and gt/eq/lt never overlap. A date is
 * "MM-DD" (every year) or "YYYY-MM-DD". Everything is nil while the clock isn't set. Literal strings are checked at
 * compile time; strings made at run time are checked when used (a bad one is a fault). */
typedef struct { int32_t s; int32_t prec; bool none; } tod_t;    /* seconds after midnight, 60 or 1 */
typedef struct { int32_t y; int32_t m, d; } date_t;               /* y 0 = every year */

static bool two_digits(const char *p, int32_t *v)
{
    if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9') return false;
    *v = (p[0] - '0') * 10 + (p[1] - '0');
    return true;
}

static const char *parse_tod(strref_t s, tod_t *t)
{
    const char *p = s.p;
    size_t n = s.len, h = n == 4 || n == 7 ? 1 : 2;       /* "H:MM", "HH:MM", "H:MM:SS", "HH:MM:SS" */
    int32_t hh = 0, mm, ss = 0;
    if (n != h + 3 && n != h + 6) return "a time is written \"HH:MM\" or \"HH:MM:SS\"";
    char hb[2] = { h == 1 ? '0' : p[0], p[h - 1] };
    if (!two_digits(hb, &hh) || p[h] != ':' || !two_digits(p + h + 1, &mm)
        || (n == h + 6 && (p[h + 3] != ':' || !two_digits(p + h + 4, &ss))))
        return "a time is written \"HH:MM\" or \"HH:MM:SS\"";
    if (hh > 23 || mm > 59 || ss > 59) return "no such time (00:00 .. 23:59:59)";
    t->s = hh * 3600 + mm * 60 + ss;
    t->prec = n == h + 6 ? 1 : 60;
    t->none = false;
    return NULL;
}

static const char *time_arg(value_t v, tod_t *t)
{
    if (v.type == V_STR) return parse_tod(v.u.str, t);
    t->none = v.u.i < 0;
    if (v.type == V_INT) { t->s = (int32_t)((int64_t)v.u.i % 1440) * 60; t->prec = 60; }
    else { t->s = (int32_t)((((int64_t)v.u.i * 60 + 32768) >> 16) % 86400); t->prec = 1; }
    return NULL;
}

static const char *parse_date(strref_t s, date_t *d)
{
    static const uint8_t mdays[12] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    const char *p = s.p;
    int32_t y = 0, yh, yl;
    if (s.len == 10) {
        if (!two_digits(p, &yh) || !two_digits(p + 2, &yl) || p[4] != '-') return "a date is written \"MM-DD\" or \"YYYY-MM-DD\"";
        y = yh * 100 + yl; p += 5;
    } else if (s.len != 5) return "a date is written \"MM-DD\" or \"YYYY-MM-DD\"";
    if (!two_digits(p, &d->m) || p[2] != '-' || !two_digits(p + 3, &d->d)) return "a date is written \"MM-DD\" or \"YYYY-MM-DD\"";
    if (d->m < 1 || d->m > 12 || d->d < 1 || d->d > mdays[d->m - 1]) return "no such date";
    if (y && d->m == 2 && d->d == 29 && !(y % 4 == 0 && (y % 100 || y % 400 == 0))) return "no such date (not a leap year)";
    if (s.len == 10 && y < 1970) return "dates start at 1970";
    d->y = y;
    return NULL;
}

/* now vs time at the time's precision: -1, 0, 1 */
static int tod_cmp(const env_t *e, const tod_t *t)
{
    int32_t now = (e->hour * 3600 + e->minute * 60 + e->second) / t->prec, at = t->s / t->prec;
    return (now > at) - (now < at);
}

static int date_cmp(const env_t *e, const date_t *d)
{
    int64_t now = (d->y ? (int64_t)e->year * 10000 : 0) + e->month * 100 + e->day;
    int64_t at = (int64_t)d->y * 10000 + d->m * 100 + d->d;
    return (now > at) - (now < at);
}

enum { CMP_GT, CMP_GE, CMP_LT, CMP_LE, CMP_EQ };
static bool cmp_holds(int r, int op)
{
    switch (op) {
    case CMP_GT: return r > 0;
    case CMP_GE: return r >= 0;
    case CMP_LT: return r < 0;
    case CMP_LE: return r <= 0;
    default:     return r == 0;
    }
}

static value_t time_cmp(exec_ctx_t *c, const value_t *a, int op)
{
    tod_t t;
    if ((c->err = time_arg(a[0], &t)) || !c->env->timeset || t.none) return boolv(false);
    return boolv(cmp_holds(tod_cmp(c->env, &t), op));
}
static value_t date_cmpv(exec_ctx_t *c, const value_t *a, int op)
{
    date_t d;
    if ((c->err = parse_date(a[0].u.str, &d)) || !c->env->timeset) return boolv(false);
    return boolv(cmp_holds(date_cmp(c->env, &d), op));
}
static value_t n_time_gt(exec_ctx_t *c, const value_t *a, uint8_t n) { (void)n; return time_cmp(c, a, CMP_GT); }
static value_t n_time_ge(exec_ctx_t *c, const value_t *a, uint8_t n) { (void)n; return time_cmp(c, a, CMP_GE); }
static value_t n_time_lt(exec_ctx_t *c, const value_t *a, uint8_t n) { (void)n; return time_cmp(c, a, CMP_LT); }
static value_t n_time_le(exec_ctx_t *c, const value_t *a, uint8_t n) { (void)n; return time_cmp(c, a, CMP_LE); }
static value_t n_time_eq(exec_ctx_t *c, const value_t *a, uint8_t n) { (void)n; return time_cmp(c, a, CMP_EQ); }
static value_t n_date_gt(exec_ctx_t *c, const value_t *a, uint8_t n) { (void)n; return date_cmpv(c, a, CMP_GT); }
static value_t n_date_ge(exec_ctx_t *c, const value_t *a, uint8_t n) { (void)n; return date_cmpv(c, a, CMP_GE); }
static value_t n_date_lt(exec_ctx_t *c, const value_t *a, uint8_t n) { (void)n; return date_cmpv(c, a, CMP_LT); }
static value_t n_date_le(exec_ctx_t *c, const value_t *a, uint8_t n) { (void)n; return date_cmpv(c, a, CMP_LE); }
static value_t n_date_eq(exec_ctx_t *c, const value_t *a, uint8_t n) { (void)n; return date_cmpv(c, a, CMP_EQ); }

/* (time-in <from> <to>): from <= now < to, wrapping past midnight when to is earlier; from = to is never. */
static value_t n_time_in(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    (void)n;
    tod_t f, t;
    if ((c->err = time_arg(a[0], &f)) || (c->err = time_arg(a[1], &t)) || !c->env->timeset || f.none || t.none)
        return boolv(false);
    int32_t now = c->env->hour * 3600 + c->env->minute * 60 + c->env->second;
    return boolv(f.s < t.s ? now >= f.s && now < t.s : f.s > t.s && (now >= f.s || now < t.s));
}

/* (date-in <from> <to>): from <= today <= to; "MM-DD" pairs wrap past the year end, "YYYY-MM-DD" pairs don't. */
static value_t n_date_in(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    (void)n;
    date_t f, t;
    if ((c->err = parse_date(a[0].u.str, &f)) || (c->err = parse_date(a[1].u.str, &t))) return boolv(false);
    if (!f.y != !t.y) { c->err = "date-in: both dates need a year, or neither"; return boolv(false); }
    if (!c->env->timeset) return boolv(false);
    int lo = date_cmp(c->env, &f), hi = date_cmp(c->env, &t);
    bool wraps = !f.y && f.m * 100 + f.d > t.m * 100 + t.d;
    return boolv(wraps ? lo >= 0 || hi <= 0 : lo >= 0 && hi <= 0);
}

/* Compile time: literal strings are checked here (the error points at the bad one); numbers can only be times. */
static const char *bind_time_cmp(node_t *n, uint8_t *bad)
{
    static char msg[96];                  /* compiles are serialized (cmd_lock on a board, one thread in WASM) */
    bool is_date = !strncmp(n->def->name, "date-", 5);
    for (uint8_t i = 0; i < n->nargs; i++) {
        const arg_t *g = &n->args[i];
        uint8_t ty = arg_type(g);
        const char *e = NULL;
        if (!is_date && !numeric(ty) && ty != V_STR) e = "a time is \"HH:MM\", \"HH:MM:SS\" or minutes after midnight";
        else if (!g->node && ty == V_STR) {
            tod_t t; date_t d;
            e = is_date ? parse_date(g->val.u.str, &d) : parse_tod(g->val.u.str, &t);
        }
        if (e) { *bad = i; snprintf(msg, sizeof msg, "%s: %s", n->def->name, e); return msg; }
    }
    if (!strcmp(n->def->name, "date-in") && !n->args[0].node && !n->args[1].node
        && (n->args[0].val.u.str.len == 10) != (n->args[1].val.u.str.len == 10)) {
        *bad = 1; return "date-in: both dates need a year, or neither";
    }
    return NULL;
}

/* (str-cat <a> <b> ...) -> string: strings joined; numbers are written as str-num writes them by default. At most
 * DSL_STR_MAX characters (the rest is cut off). */
static value_t n_str_cat(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    size_t total = 0;
    for (uint8_t i = 0; i < n; i++) total += a[i].type == V_STR ? a[i].u.str.len : 48;
    if (total > DSL_STR_MAX) total = DSL_STR_MAX;
    char *p = str_alloc(c, total);
    if (!p) return strv("", 0);
    size_t o = 0;
    for (uint8_t i = 0; i < n && o < total; i++) {
        char num[48];
        const char *src = num;
        size_t len;
        if (a[i].type == V_STR) { src = a[i].u.str.p; len = a[i].u.str.len; }
        else len = (size_t)fmt_num(num, a[i], 0, a[i].type == V_FIX ? 2 : 0, false);
        if (len > total - o) len = total - o;
        memcpy(p + o, src, len);
        o += len;
    }
    return strv(p, o);
}

static const char *bind_str_cat(node_t *n, uint8_t *bad)
{
    for (uint8_t i = 0; i < n->nargs; i++) {
        uint8_t t = arg_type(&n->args[i]);
        if (t != V_STR && !numeric(t)) { *bad = i; return "str-cat joins strings and numbers"; }
    }
    return NULL;
}

/* (str-sub <s> [start] [end]) -> part of s (the usual range rules, counted in characters); no copy */
static value_t n_str_sub(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    (void)c;
    strref_t s = a[0].u.str;
    int32_t st = range_index(opt_int(a, n, 1, 0), s.len), en = range_index(opt_int(a, n, 2, s.len), s.len);
    return st < en ? strv(s.p + st, (size_t)(en - st)) : strv(s.p, 0);
}

static value_t n_str_len(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    (void)c; (void)n;
    value_t v = { .type = V_INT };
    v.u.i = a[0].u.str.len;
    return v;
}

/* ---- segment displays: a display wired as one strip, written as a mask ---- */

/* 7-segment font: bit 0..6 = segments a..g (a top, clockwise, g middle), bit 7 = decimal point. The displayable
 * subset of ASCII (common 7-segment alphabets; S=5, Z=2 style approximations included, K M W X and the like are not
 * displayable and stay blank). Letters fall back to the other case when only one form exists. */
static const uint8_t k_seg7[128] = {
    ['0'] = 0x3F, ['1'] = 0x06, ['2'] = 0x5B, ['3'] = 0x4F, ['4'] = 0x66, ['5'] = 0x6D, ['6'] = 0x7D, ['7'] = 0x07,
    ['8'] = 0x7F, ['9'] = 0x6F,
    ['A'] = 0x77, ['a'] = 0x5F, ['b'] = 0x7C, ['C'] = 0x39, ['c'] = 0x58, ['d'] = 0x5E, ['E'] = 0x79, ['F'] = 0x71,
    ['G'] = 0x3D, ['H'] = 0x76, ['h'] = 0x74, ['I'] = 0x30, ['i'] = 0x10, ['J'] = 0x1E, ['L'] = 0x38, ['l'] = 0x30,
    ['N'] = 0x37, ['n'] = 0x54, ['O'] = 0x3F, ['o'] = 0x5C, ['P'] = 0x73, ['q'] = 0x67, ['r'] = 0x50, ['S'] = 0x6D,
    ['t'] = 0x78, ['U'] = 0x3E, ['u'] = 0x1C, ['V'] = 0x3E, ['Y'] = 0x6E, ['y'] = 0x6E, ['Z'] = 0x5B,
    [' '] = 0x00, ['-'] = 0x40, ['_'] = 0x08, ['='] = 0x48, ['"'] = 0x22, ['\''] = 0x02, ['('] = 0x39, [')'] = 0x0F,
    ['['] = 0x39, [']'] = 0x0F, ['/'] = 0x52, ['?'] = 0x53, ['^'] = 0x23, ['*'] = 0x63, ['.'] = 0x80, [','] = 0x80,
};

/* 9-segment: 7-segment plus two centre verticals, j (top, between f and b) and m (bottom, between e and c), the
 * names 14-segment displays give them. Bits a..g 0..6, j 7, m 8, p 9. Not a standard display, so this is our own
 * table of what the centre verticals improve (T I M W K R Y B D Q and a few symbols); everything else is the
 * 7-segment glyph. */
static const uint16_t k_seg9[128] = {
    ['B'] = 0x1CF, ['D'] = 0x18F, ['I'] = 0x189, ['K'] = 0x0F4, ['M'] = 0x0B7, ['Q'] = 0x13F, ['R'] = 0x173,
    ['T'] = 0x181, ['W'] = 0x13E, ['X'] = 0x076, ['Y'] = 0x162, ['i'] = 0x100, ['m'] = 0x154, ['w'] = 0x11C,
    ['+'] = 0x1C0, ['|'] = 0x180, ['!'] = 0x280, ['$'] = 0x1ED,
};

/* 14-segment ("starburst"): bits a b c d e f g1 g2 h j k l m n p = 0..14, with the datasheet names: g1/g2 the left and
 * right halves of the middle bar, h j k the upper diagonal / centre / diagonal (left to right), n m l the lower ones
 * (left to right). Upper case and symbols; lower case shows as upper case. */
static const uint16_t k_seg14[128] = {
    ['0'] = 0x003F, ['1'] = 0x0006, ['2'] = 0x00DB, ['3'] = 0x008F, ['4'] = 0x00E6, ['5'] = 0x00ED, ['6'] = 0x00FD,
    ['7'] = 0x0007, ['8'] = 0x00FF, ['9'] = 0x00EF,
    ['A'] = 0x00F7, ['B'] = 0x128F, ['C'] = 0x0039, ['D'] = 0x120F, ['E'] = 0x0079, ['F'] = 0x0071, ['G'] = 0x00BD,
    ['H'] = 0x00F6, ['I'] = 0x1209, ['J'] = 0x001E, ['K'] = 0x0C70, ['L'] = 0x0038, ['M'] = 0x0536, ['N'] = 0x0936,
    ['O'] = 0x003F, ['P'] = 0x00F3, ['Q'] = 0x083F, ['R'] = 0x08F3, ['S'] = 0x00ED, ['T'] = 0x1201, ['U'] = 0x003E,
    ['V'] = 0x2430, ['W'] = 0x2836, ['X'] = 0x2D00, ['Y'] = 0x1500, ['Z'] = 0x2409,
    ['!'] = 0x4006, ['"'] = 0x0202, ['#'] = 0x12CE, ['$'] = 0x12ED, ['%'] = 0x2424, ['&'] = 0x0B59, ['\''] = 0x0200,
    ['('] = 0x0C00, [')'] = 0x2100, ['*'] = 0x3FC0, ['+'] = 0x12C0, [','] = 0x2000, ['-'] = 0x00C0, ['.'] = 0x4000,
    ['/'] = 0x2400, [':'] = 0x1200, [';'] = 0x2200, ['<'] = 0x0C00, ['='] = 0x00C8, ['>'] = 0x2100, ['?'] = 0x1083,
    ['@'] = 0x02BB, ['['] = 0x0039, ['\\'] = 0x0900, [']'] = 0x000F, ['^'] = 0x2800, ['_'] = 0x0008, ['`'] = 0x0100,
    ['{'] = 0x2149, ['|'] = 0x1200, ['}'] = 0x0C89,
};

static char other_case(unsigned char u) { return (char)(u >= 'a' && u <= 'z' ? u - 32 : u >= 'A' && u <= 'Z' ? u + 32 : u); }
static uint16_t seg7_to9(uint8_t g) { return (uint16_t)((g & 0x7Fu) | (g & 0x80u) << 2); }   /* point: bit 7 -> 9 */

uint16_t dsl_seg_glyph(int kind, char ch)
{
    unsigned char u = (unsigned char)ch, o = (unsigned char)other_case(u);
    if (u >= 128) return 0;
    switch (kind) {
    case 7:  return k_seg7[u] ? k_seg7[u] : k_seg7[o];
    case 9:  return k_seg9[u] ? k_seg9[u] : k_seg7[u] ? seg7_to9(k_seg7[u]) : k_seg9[o] ? k_seg9[o] : seg7_to9(k_seg7[o]);
    case 14: return k_seg14[u] ? k_seg14[u] : k_seg14[o];
    }
    return 0;
}

/* The three displays: segment names (bit order, p last) and which stride each segment's length is. */
enum { SL_H, SL_V, SL_D, SL_G, SL_P };
typedef struct {
    uint8_t kind, nseg;
    const char *names[15];
    uint8_t len[15];                                      /* SL_* */
    uint8_t first_digits;                                 /* argument index of [digits]; strides are 3.. before it */
} segdisp_t;
static const segdisp_t k_seg7d  = { 7, 8, { "a", "b", "c", "d", "e", "f", "g", "p" },
                                    { SL_H, SL_V, SL_V, SL_H, SL_V, SL_V, SL_H, SL_P }, 5 };
static const segdisp_t k_seg9d  = { 9, 10, { "a", "b", "c", "d", "e", "f", "g", "j", "m", "p" },
                                    { SL_H, SL_V, SL_V, SL_H, SL_V, SL_V, SL_H, SL_V, SL_V, SL_P }, 5 };
static const segdisp_t k_seg14d = { 14, 15, { "a", "b", "c", "d", "e", "f", "g1", "g2", "h", "j", "k", "l", "m", "n", "p" },
                                    { SL_H, SL_V, SL_V, SL_H, SL_V, SL_V, SL_G, SL_G, SL_D, SL_V, SL_D, SL_D, SL_V, SL_D, SL_P }, 7 };

/* Layout: the order the strip passes through the segments, written as their names run together (abcdefg; on 14
 * segments a b c d e f g1 g2 h j k l m n), each once, p (decimal point) optional. Rewritten in place at compile time
 * into segment numbers + 1 (each name is at least one character, so it fits), NUL-terminated. */
static bool seg_layout(const segdisp_t *sd, char *s)
{
    uint32_t seen = 0;
    char *out = s;
    while (*s) {
        int best = -1; size_t blen = 0;
        for (int k = 0; k < sd->nseg; k++) {
            size_t l = strlen(sd->names[k]), i = 0;
            while (i < l && s[i] && (s[i] | 0x20) == sd->names[k][i]) i++;
            if (i == l && l > blen) { best = k; blen = l; }
        }
        if (best < 0 || (seen & (1u << best))) return false;
        seen |= 1u << best;
        s += blen;
        *out++ = (char)(best + 1);
    }
    *out = 0;
    uint32_t need = (1u << (sd->nseg - 1)) - 1;          /* every segment but p */
    return (seen & need) == need;
}

static const char *bind_seg(const segdisp_t *sd, node_t *n, uint8_t *bad)
{
    uint8_t t = arg_type(&n->args[1]);
    if (t != V_INT && t != V_STR) {
        *bad = 1;
        return t == V_FIX ? "expected an int or a string (round it, or make text with str-num)" : "expected an int or a string";
    }
    if (n->nargs > 2 && !n->args[2].node && n->args[2].val.type == V_SYM && !seg_layout(sd, (char *)n->args[2].val.u.sym)) {
        *bad = 2;
        return sd->kind == 14 ? "layout: the order of the segments along the strip, each of a b c d e f g1 g2 h j k l m n once (p = decimal point)"
             : sd->kind == 9 ? "layout: the order of the segments along the strip, each of a..g j m once (p = decimal point), e.g. abcdefgjm"
             : "layout: the order of the segments along the strip, each of a..g once (p = decimal point), e.g. abcdefg";
    }
    for (uint8_t i = 3; i <= sd->first_digits && i < n->nargs; i++)
        if (!n->args[i].node && n->args[i].val.type == V_INT && n->args[i].val.u.i < 1) {
            *bad = i;
            return i == sd->first_digits ? "digits must be at least 1" : "strides must be at least 1 pixel";
        }
    return NULL;
}
static const char *bind_seg7(node_t *n, uint8_t *bad)  { return bind_seg(&k_seg7d, n, bad); }
static const char *bind_seg9(node_t *n, uint8_t *bad)  { return bind_seg(&k_seg9d, n, bad); }
static const char *bind_seg14(node_t *n, uint8_t *bad) { return bind_seg(&k_seg14d, n, bad); }

#define SEG_DIGITS_MAX 16

/* (segN <dst> <number|text> [layout] [hstride] [vstride] ([dstride] [gstride]) [digits]) -> dst
 * Writes a mask on a segment display wired as one strip: lit segment pixels #FFFFFFFF, the others black. Each digit is
 * the layout's segments in order, each as long as its stride (a d: hstride; b c e f j m: vstride; 14: diagonals
 * dstride, g1 g2 gstride; p 1 pixel); digits follow each other from the start of dst, as many as fit unless given.
 * Numbers are right-aligned with blanks (negatives get a minus; too long: dashes); text is left-aligned and cut off,
 * and a '.' or ',' after a character lights that digit's point. Pixels after the last digit are untouched. */
static value_t seg_draw(const segdisp_t *sd, exec_ctx_t *c, const value_t *a, uint8_t n)
{
    static const char k_default[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 0 };
    const bufref_t *d = &a[0].u.buf;
    char deflt[16];
    const char *layout = deflt;
    if (n > 2 && a[2].type == V_SYM) layout = a[2].u.sym;
    else { memcpy(deflt, k_default, (size_t)sd->nseg - 1); deflt[sd->nseg - 1] = 0; }   /* every segment but p */
    int32_t hs = opt_int(a, n, 3, 1), vs = opt_int(a, n, 4, 1), ds = vs, gs = (hs + 1) / 2;
    if (sd->kind == 14) { ds = opt_int(a, n, 5, vs); gs = opt_int(a, n, 6, (hs + 1) / 2); }
    if (hs < 1 || vs < 1 || ds < 1 || gs < 1) { c->err = "strides must be at least 1 pixel"; return a[0]; }
    const uint32_t lens[] = { (uint32_t)hs, (uint32_t)vs, (uint32_t)ds, (uint32_t)gs, 1 };
    uint32_t dlen = 0;
    for (const char *l = layout; *l; l++) dlen += lens[sd->len[*l - 1]];
    int32_t ndig = opt_int(a, n, sd->first_digits, (int32_t)(d->len / dlen));
    if (ndig < 1) { if (n > sd->first_digits && a[sd->first_digits].type != V_NIL) c->err = "digits must be at least 1"; return a[0]; }
    if (ndig > SEG_DIGITS_MAX) ndig = SEG_DIGITS_MAX;

    uint16_t glyph[SEG_DIGITS_MAX] = { 0 };
    uint16_t point = (uint16_t)(1u << (sd->nseg - 1));
    if (a[1].type == V_INT) {                             /* right-aligned with blanks; dashes if it doesn't fit */
        char num[12];
        int len = snprintf(num, sizeof num, "%ld", (long)a[1].u.i);
        for (int32_t k = 0; k < ndig; k++)
            glyph[k] = len > ndig ? dsl_seg_glyph(sd->kind, '-') : k >= ndig - len ? dsl_seg_glyph(sd->kind, num[k - (ndig - len)]) : 0;
    } else {                                              /* text: left-aligned, points join the character before */
        strref_t t = a[1].u.str;
        int32_t k = 0;
        bool joined = true;                               /* the digit before can't take a point */
        for (uint16_t i = 0; i < t.len; i++) {
            if ((t.p[i] == '.' || t.p[i] == ',') && !joined) { glyph[k - 1] |= point; joined = true; continue; }
            if (k == ndig) break;
            glyph[k++] = dsl_seg_glyph(sd->kind, t.p[i]);
            joined = (glyph[k - 1] & point) != 0;
        }
    }
    for (int32_t dg = 0; dg < ndig; dg++) {
        uint32_t at = (uint32_t)dg * dlen;
        for (const char *l = layout; *l; l++) {
            uint32_t seg = (uint32_t)(*l - 1), len = lens[sd->len[seg]];
            pixel_t px = (glyph[dg] >> seg) & 1u ? 0xFFFFFFFFu : 0u;
            for (uint32_t p = 0; p < len && at + p < d->len; p++) *buf_at(d, (int32_t)(at + p)) = px;
            at += len;
        }
    }
    return a[0];
}
static value_t n_seg7(exec_ctx_t *c, const value_t *a, uint8_t n)  { return seg_draw(&k_seg7d, c, a, n); }
static value_t n_seg9(exec_ctx_t *c, const value_t *a, uint8_t n)  { return seg_draw(&k_seg9d, c, a, n); }
static value_t n_seg14(exec_ctx_t *c, const value_t *a, uint8_t n) { return seg_draw(&k_seg14d, c, a, n); }

/* ---- automation: the system's side (dsl_host_t), only in the automation program (NF_AUTOMATION) ---- */

static const dsl_host_t *s_host;
const dsl_host_t *dsl_host(void) { return s_host; }
void dsl_set_host(const dsl_host_t *h) { s_host = h; }

/* Channel argument: NULL if fine, else the fault. */
static const char *check_ch(int32_t ch)
{
    if (!s_host || !s_host->nchannels) return "automation functions aren't available here";
    return ch >= 0 && ch < s_host->nchannels() ? NULL : "no such channel";
}

/* (intensity) -> global, (intensity ch) -> that channel; decimal 0..1 (where a fade is now) */
static value_t n_intensity(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    value_t v = { .type = V_NIL };
    int ch = n == 0 || a[0].type == V_NIL ? -1 : a[0].u.i;
    if (!s_host || !s_host->intensity_get) { c->err = "automation functions aren't available here"; return v; }
    if (ch >= 0 && (c->err = check_ch(ch))) return v;
    v.type = V_FIX;
    v.u.i = (int32_t)s_host->intensity_get(ch) << 8;          /* 0..256 -> Q16.16 0..1 */
    return v;
}

/* (set-intensity <ch | _> <0..1> [fade seconds]): _ = the global intensity; fades happen frame by frame */
static value_t n_set_intensity(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    value_t v = { .type = V_NIL };
    int ch = a[0].type == V_NIL ? -1 : a[0].u.i;
    if (!s_host || !s_host->intensity_set) { c->err = "automation functions aren't available here"; return v; }
    if (ch >= 0 && (c->err = check_ch(ch))) return v;
    int64_t fade = n > 2 && a[2].type != V_NIL ? q16(a[2]) : 0;       /* seconds, Q16.16 */
    if (fade < 0) { c->err = "fade must not be negative"; return v; }
    if (fade > (int64_t)3600 * 65536) fade = (int64_t)3600 * 65536;
    s_host->intensity_set(ch, (uint16_t)a[1].u.i, (uint32_t)((fade * 1000 + 32768) >> 16));
    return v;
}

/* Library names, as the system stores them: 1..15 chars, a-z 0-9 - _, starting with a letter. */
static bool lib_name_ok(const char *s)
{
    size_t n = strlen(s);
    if (!n || n > 15 || s[0] < 'a' || s[0] > 'z') return false;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-' || s[i] == '_')) return false;
    return true;
}

/* The name is checked when the program compiles (like env names), so a typo is a compile error. */
static const char *bind_play(node_t *n, uint8_t *bad)
{
    const char *name = n->args[1].val.u.sym;
    *bad = 1;
    if (!lib_name_ok(name)) return "library names are lower case a-z 0-9 - _, starting with a letter, up to 15";
    if (s_host && s_host->lib_exists && !s_host->lib_exists(name)) return "no library program by that name (lib to list them)";
    return NULL;
}

/* (play ch name): run that library program on the channel (nothing happens if it already runs there) */
static value_t n_play(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    (void)n;
    value_t v = { .type = V_NIL };
    if ((c->err = check_ch(a[0].u.i))) return v;
    if (!s_host->play) { c->err = "automation functions aren't available here"; return v; }
    c->err = s_host->play(a[0].u.i, a[1].u.sym);
    return v;
}

/* (stop ch): the channel goes dark (its program is replaced by one that clears O) */
static value_t n_stop(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    (void)n;
    value_t v = { .type = V_NIL };
    if ((c->err = check_ch(a[0].u.i))) return v;
    if (!s_host->stop) { c->err = "automation functions aren't available here"; return v; }
    c->err = s_host->stop(a[0].u.i);
    return v;
}

/* ---- GPIO outputs: (io out) and (io-set out value), out = a slot number or the output's name ---- */

/* A name is turned into its slot number when the program compiles, so a typo is a compile error. */
/* The slot argument of io / io-set / in / analog: a bare name becomes the slot number at compile time; a constant
 * slot must exist; when the host knows the slot's mode it must be the kind the native works with. */
enum { WANT_OUTPUT, WANT_INPUT, WANT_ANALOG };
static const char *bind_slot(node_t *n, int want)
{
    static const char *const what[] = { "output", "input", "analog input" };
    static char msg[96];                  /* compiles are serialized (cmd_lock on a board, one thread in WASM) */
    arg_t *o = &n->args[0];
    if (!o->node && o->val.type == V_SYM) {
        int slot = s_host && s_host->io_find ? s_host->io_find(o->val.u.sym) : -1;
        if (slot < 0) { snprintf(msg, sizeof msg, "no %s by that name (io to list them)", what[want]); return msg; }
        o->val.type = V_INT;
        o->val.u.i = slot;
    } else if (!o->node && s_host && s_host->io_used && !s_host->io_used(o->val.u.i)) {
        snprintf(msg, sizeof msg, "no such %s (io to list them)", what[want]);
        return msg;
    }
    int m = !o->node && s_host && s_host->io_mode ? s_host->io_mode(o->val.u.i) : -1;
    if (m >= 0) {
        bool ok = want == WANT_OUTPUT ? (m == DSL_IO_BINARY || m == DSL_IO_PWM) : want == WANT_INPUT ? m == DSL_IO_INPUT : m == DSL_IO_ANALOG;
        if (!ok) {
            static const char *const hint[] = { "read it with (in x) or (analog x)", "", "" };
            const char *mode = m == DSL_IO_INPUT ? "a digital input" : m == DSL_IO_ANALOG ? "an analog input"
                             : m == DSL_IO_AUDIO ? "an audio input" : "an output";
            if (m == DSL_IO_AUDIO) snprintf(msg, sizeof msg, "that is an audio input: its values are (env band0) .. (env beatms)");
            else if (want == WANT_OUTPUT) snprintf(msg, sizeof msg, "that is %s: %s", mode, hint[0]);
            else snprintf(msg, sizeof msg, "that is %s, not an %s", mode, what[want]);
            return msg;
        }
    }
    return NULL;
}

static const char *bind_io(node_t *n, uint8_t *bad)
{
    *bad = 0;
    const char *why = bind_slot(n, WANT_OUTPUT);
    if (why) return why;
    if (n->nargs > 1) {
        uint8_t t = arg_type(&n->args[1]);
        *bad = 1;
        if (t != V_BOOL && !numeric(t)) return "expected t, nil or a level 0..1";
    }
    return NULL;
}

static const char *check_io(int32_t n)
{
    if (!s_host || !s_host->io_used || !s_host->io_get || !s_host->io_set) return "outputs aren't available here";
    if (!s_host->io_used(n)) return "no such output";
    int m = s_host->io_mode ? s_host->io_mode(n) : -1;
    return m == DSL_IO_INPUT || m == DSL_IO_ANALOG || m == DSL_IO_AUDIO ? "that is an input now (io-set/io are for outputs)" : NULL;
}

/* (in x) -> t / nil: a digital input, debounced, after invert. Fine in channel programs: it reads a cached value. */
static const char *bind_in(node_t *n, uint8_t *bad) { *bad = 0; return bind_slot(n, WANT_INPUT); }
static value_t n_in(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    (void)n;
    int32_t s = a[0].u.i;
    if (!s_host || !s_host->in_get || !s_host->io_used) { c->err = "inputs aren't available here"; return boolv(false); }
    if (!s_host->io_used(s)) { c->err = "no such input"; return boolv(false); }
    int m = s_host->io_mode ? s_host->io_mode(s) : DSL_IO_INPUT;
    if (m >= 0 && m != DSL_IO_INPUT) { c->err = "that is not a digital input now"; return boolv(false); }
    return boolv(s_host->in_get(s));
}

/* (analog x [samples]) -> decimal 0..1: the average of the latest `samples` readings (one per ms; default 16). */
static const char *bind_analog(node_t *n, uint8_t *bad)
{
    *bad = 0;
    const char *why = bind_slot(n, WANT_ANALOG);
    if (why) return why;
    if (n->nargs > 1 && !n->args[1].node && n->args[1].val.type == V_INT
        && (n->args[1].val.u.i < 1 || n->args[1].val.u.i > DSL_ANALOG_SAMPLES_MAX)) {
        *bad = 1;
        return "analog: samples must be 1..256";
    }
    return NULL;
}
static value_t n_analog(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    int32_t s = a[0].u.i, k = opt_int(a, n, 1, DSL_ANALOG_SAMPLES_DEFAULT);
    value_t v = { .type = V_FIX };
    if (!s_host || !s_host->analog_get || !s_host->io_used) { c->err = "inputs aren't available here"; return v; }
    if (!s_host->io_used(s)) { c->err = "no such input"; return v; }
    int m = s_host->io_mode ? s_host->io_mode(s) : DSL_IO_ANALOG;
    if (m >= 0 && m != DSL_IO_ANALOG) { c->err = "that is not an analog input now"; return v; }
    if (k < 1) k = 1;
    if (k > DSL_ANALOG_SAMPLES_MAX) k = DSL_ANALOG_SAMPLES_MAX;
    uint32_t r = s_host->analog_get(s, (int)k);
    v.u.i = (int32_t)(r > 65536 ? 65536 : r);
    return v;
}

/* (io out) -> where the output is: 0..1 (binary: 0 or 1) */
static value_t n_io(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    (void)n;
    value_t v = { .type = V_NIL };
    if ((c->err = check_io(a[0].u.i))) return v;
    v.type = V_FIX;
    v.u.i = (int32_t)s_host->io_get(a[0].u.i) << 8;
    return v;
}

/* (io-set out <t | nil | 0..1>): t / nil switch it, a level sets a PWM duty (binary outputs: anything above 0 = on) */
static value_t n_io_set(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    (void)n;
    value_t v = { .type = V_NIL };
    if ((c->err = check_io(a[0].u.i))) return v;
    int64_t lv;
    switch (a[1].type) {
    case V_BOOL: lv = a[1].u.i ? 256 : 0; break;
    case V_INT:  lv = (int64_t)a[1].u.i * 256; break;
    case V_FIX:  lv = ((int64_t)a[1].u.i * 256 + 32768) >> 16; break;
    default: c->err = "io-set: expected t, nil or a level 0..1"; return v;
    }
    if (lv < 0) lv = 0;
    if (lv > 256) lv = 256;
    if (a[1].type == V_FIX && a[1].u.i > 0 && lv == 0) lv = 1;   /* a tiny level is still on for a binary output */
    c->err = s_host->io_set(a[0].u.i, (uint16_t)lv);
    return v;
}

/* ---- variables (emitted by the compiler, see dsl_internal.h) ---- */
static value_t n_var_set(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    (void)n;
    value_t nil = { .type = V_NIL };
    if (c->vars) c->vars[a[0].u.i] = a[1];
    return nil;
}
static value_t n_var_get(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    (void)n;
    value_t nil = { .type = V_NIL };
    return c->vars ? c->vars[a[0].u.i] : nil;            /* never set this run -> V_NIL -> run-time type fault */
}
const native_def_t dsl_var_set = { "def", n_var_set, V_NIL, 2, 2, { A_INT, A_ANY }, 0, NULL };
const native_def_t dsl_var_get = { "variable", n_var_get, V_NIL, 1, 1, { A_INT }, 0, NULL };   /* node->ret = var type */
static value_t n_const(exec_ctx_t *c, const value_t *a, uint8_t n) { (void)c; (void)n; return a[0]; }
const native_def_t dsl_const = { "value", n_const, V_NIL, 1, 1, { A_ANY }, 0, NULL };          /* node->ret = its type */

/* ---- calling user functions ---- */

/* The scratch used by one call of f is given back after it unless f stores a string in an enclosing variable. */
static size_t scratch_mark(const exec_ctx_t *c) { return c->scratch ? c->scratch->used : 0; }
static void scratch_back(exec_ctx_t *c, const ufn_t *f, size_t mark) { if (c->scratch && !(f->flags & UF_KEEP)) c->scratch->used = mark; }

/* (map-px <dst> <src> <f> [start] [end]) -> dst: dst[i] = (f src[i] i) for i in the range, taken over the shorter view
 * (i counts from the views' start). f takes (c:color) or (c:color i:int) and returns a color. dst[i] is written as soon
 * as f returns; src is read as if completely before any write (staged in scratch when the views overlap, unless they
 * are the same view: then f sees the pixels it already wrote if it reads them with get). */
static value_t n_map_px(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    const bufref_t *d = &a[0].u.buf, *s = &a[1].u.buf;
    const ufn_t *f = a[2].u.fn;
    bufref_t both = { .px = NULL, .len = d->len < s->len ? d->len : s->len, .stride = 1 };
    int32_t st, en;
    if (!range(&both, a, n, 3, &st, &en)) return a[0];
    int32_t cnt = en - st;
    pixel_t *dp = buf_at(d, st);
    const pixel_t *sp = buf_at(s, st);
    ptrdiff_t ds = d->stride, ss = s->stride;
    uintptr_t dlo, dhi, slo, shi;
    view_bounds(dp, ds, cnt, &dlo, &dhi);
    view_bounds(sp, ss, cnt, &slo, &shi);
    if (dlo <= shi && slo <= dhi && !(dp == sp && ds == ss)) {
        const pixel_t *tmp = stage(c, sp, ss, cnt);
        if (!tmp) { c->err = "not enough scratch memory for overlapping buffers"; return a[0]; }
        sp = tmp; ss = 1;
    }
    size_t mark = scratch_mark(c);
    value_t argv[2] = { { .type = V_COLOR }, { .type = V_INT } };
    for (int32_t i = 0; i < cnt; i++) {
        argv[0].u.color = sp[(ptrdiff_t)i * ss];
        argv[1].u.i = i + st;
        value_t v = dsl_call(c, f, argv);
        if (c->err) return a[0];
        dp[(ptrdiff_t)i * ds] = v.u.color;
        scratch_back(c, f, mark);
    }
    return a[0];
}
static const char *bind_map_px(node_t *n, uint8_t *bad)
{
    const ufn_t *f = n->args[2].val.u.fn;
    *bad = 2;
    if (f->nparams < 1 || f->nparams > 2 || f->def.args[0] != A_COLOR || (f->nparams == 2 && f->def.args[1] != A_INT))
        return "map-px needs a function of (c:color) or (c:color i:int)";
    if (f->def.ret != V_COLOR) return "map-px needs a function that returns a color";
    return NULL;
}

/* (repeat <n> <f>): calls f n times, with i = 0 .. n-1 if it takes (i:int); nothing if n <= 0. Returns nothing. */
static value_t n_repeat(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    (void)n;
    const ufn_t *f = a[1].u.fn;
    value_t argv[1] = { { .type = V_INT } }, nil = { .type = V_NIL };
    size_t mark = scratch_mark(c);
    for (int32_t i = 0; i < a[0].u.i; i++) {
        argv[0].u.i = i;
        dsl_call(c, f, argv);
        if (c->err) break;
        scratch_back(c, f, mark);
    }
    return nil;
}
static const char *bind_repeat(node_t *n, uint8_t *bad)
{
    const ufn_t *f = n->args[1].val.u.fn;
    *bad = 1;
    if (f->nparams > 1 || (f->nparams == 1 && f->def.args[0] != A_INT)) return "repeat needs a function of (i:int) or ()";
    return NULL;
}

/* ---- repeat a pattern ---- */

/* (fill-pat <dst> <pattern> [phase] [stretch] [mirror] [start] [end]) -> dst
 * The repeated sequence is the pattern, in ping-pong order if mirror (ABC -> ABCCBA), with every pixel repeated
 * `stretch` times (default 1). dst[start + i] = seq[(i - phase) mod len(seq)]: positive phase moves it right, like rotate.
 * Empty pattern: no-op. A pattern inside the range being written is read from a copy (staged in scratch). */
static value_t n_fill_pat(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    const bufref_t *d = &a[0].u.buf;
    bufref_t pat = a[1].u.buf;
    int32_t phase = opt_int(a, n, 2, 0), stretch = opt_int(a, n, 3, 1);
    bool mirror = n > 4 && a[4].type == V_BOOL && a[4].u.i;
    if (stretch < 1) { c->err = "fill-pat stretch must be at least 1"; return a[0]; }
    int32_t s, e;
    if (!range(d, a, n, 5, &s, &e) || pat.len == 0) return a[0];
    int32_t cnt = e - s;
    pixel_t *dp = buf_at(d, s);
    uintptr_t dlo, dhi, plo, phi;
    view_bounds(dp, d->stride, cnt, &dlo, &dhi);
    view_bounds(pat.px, pat.stride, pat.len, &plo, &phi);
    if (dlo <= phi && plo <= dhi) {
        pixel_t *tmp = stage(c, pat.px, pat.stride, pat.len);
        if (!tmp) { c->err = "not enough scratch memory for overlapping buffers"; return a[0]; }
        pat.px = tmp; pat.stride = 1;
    }
    int32_t len = pat.len, steps = mirror ? 2 * len : len;      /* pattern positions per period, before stretching */
    int64_t j0 = floormod64(-(int64_t)phase, (int64_t)steps * stretch);
    int32_t q = (int32_t)(j0 / stretch), r = (int32_t)(j0 % stretch);   /* position in sequence, repeat counter */
    for (ptrdiff_t o = 0, end = (ptrdiff_t)cnt * d->stride; o != end; o += d->stride) {
        dp[o] = *buf_at(&pat, q < len ? q : 2 * len - 1 - q);
        if (++r == stretch) { r = 0; if (++q == steps) q = 0; }
    }
    return a[0];
}

/* A constant stretch below 1 is a compile error, not a fault on every frame. */
static const char *bind_fill_pat(node_t *n, uint8_t *bad)
{
    if (n->nargs > 3 && !n->args[3].node && n->args[3].val.type == V_INT && n->args[3].val.u.i < 1) {
        *bad = 3;
        return "fill-pat stretch must be at least 1";
    }
    return NULL;
}

static const native_def_t k_natives[] = {
    { "buf",  n_buf,  V_BUF, 1, 1, { A_SYM },                          NF_FOLD, NULL },
    { "slice", n_slice, V_BUF, 1, 4, { A_BUF, A_INT, A_INT, A_INT },  NF_FOLD, NULL },
    { "fill", n_fill, V_BUF, 2, 4, { A_BUF, A_COLOR, A_INT, A_INT },   0, NULL },
    { "scale", n_scale, V_BUF, 2, 4, { A_BUF, A_UNIT,  A_INT, A_INT },  0, NULL },
    { "invert", n_invert, V_BUF, 1, 3, { A_BUF, A_INT, A_INT },         0, NULL },
    { "copy",   n_copy,   V_BUF, 2, 4, { A_BUF, A_BUF, A_INT, A_INT },  0, NULL },
    { "screen", n_screen, V_BUF, 2, 4, { A_BUF, A_BUF, A_INT, A_INT },  0, NULL },
    { "add",    n_add,    V_BUF, 2, 4, { A_BUF, A_BUF, A_INT, A_INT },  0, NULL },
    { "subtract", n_subtract, V_BUF, 2, 4, { A_BUF, A_BUF, A_INT, A_INT }, 0, NULL },
    { "lighten", n_lighten, V_BUF, 2, 4, { A_BUF, A_BUF, A_INT, A_INT }, 0, NULL },
    { "darken", n_darken, V_BUF, 2, 4, { A_BUF, A_BUF, A_INT, A_INT },  0, NULL },
    { "multiply", n_multiply, V_BUF, 2, 4, { A_BUF, A_BUF, A_INT, A_INT }, 0, NULL },
    { "mix",    n_mix,    V_BUF, 3, 5, { A_BUF, A_BUF, A_UNIT, A_INT, A_INT }, 0, NULL },
    { "grad-map", n_grad_map, V_BUF, 3, 5, { A_BUF, A_BUF, A_BUF, A_INT, A_INT },             0, NULL },
    { "fill-pat", n_fill_pat, V_BUF, 2, 7, { A_BUF, A_BUF, A_INT, A_INT, A_BOOL, A_INT, A_INT }, 0, bind_fill_pat },
    { "fill-grad", n_fill_grad, V_BUF, 2, 4, { A_BUF, A_BUF, A_INT, A_INT },                     0, NULL },
    { "fill-rand", n_fill_rand, V_BUF, 4, 6, { A_BUF, A_COLOR, A_UNIT, A_UNIT, A_INT, A_INT },   0, NULL },
    { "fill-pick", n_fill_pick, V_BUF, 4, 6, { A_BUF, A_COLOR, A_COLOR, A_UNIT, A_INT, A_INT },  0, NULL },
    { "set",    n_set,    V_BUF,   3, 10, { A_BUF, A_INT, A_COLOR, A_COLOR, A_COLOR, A_COLOR,
                                           A_COLOR, A_COLOR, A_COLOR, A_COLOR },                 0, NULL },
    { "get",    n_get,    V_COLOR, 2, 2,  { A_BUF, A_INT },                                       0, NULL },
    { "rotate", n_rotate, V_BUF,   2, 4,  { A_BUF, A_NUM, A_INT, A_INT },                         0, NULL },
    { "map-px", n_map_px, V_BUF,   3, 5,  { A_BUF, A_BUF, A_FN, A_INT, A_INT },                   0, bind_map_px },
    { "repeat", n_repeat, V_NIL,   2, 2,  { A_INT, A_FN },                                        0, bind_repeat },
    { "if",     n_if,     V_NIL,   2, 3,  { A_BOOL, A_ANY, A_ANY },                               NF_LAZY, bind_if },
    { "and",    n_and,    V_BOOL,  2, 10, { A_BOOL, A_BOOL, A_BOOL, A_BOOL, A_BOOL,
                                           A_BOOL, A_BOOL, A_BOOL, A_BOOL, A_BOOL },               NF_LAZY, NULL },
    { "or",     n_or,     V_BOOL,  2, 10, { A_BOOL, A_BOOL, A_BOOL, A_BOOL, A_BOOL,
                                           A_BOOL, A_BOOL, A_BOOL, A_BOOL, A_BOOL },               NF_LAZY, NULL },
    { "not",    n_not,    V_BOOL,  1, 1,  { A_BOOL },                                             NF_FOLD, NULL },
    { "lt",     n_lt,     V_BOOL,  2, 2,  { A_NUMERIC, A_NUMERIC },                               NF_FOLD, NULL },
    { "gt",     n_gt,     V_BOOL,  2, 2,  { A_NUMERIC, A_NUMERIC },                               NF_FOLD, NULL },
    { "le",     n_le,     V_BOOL,  2, 2,  { A_NUMERIC, A_NUMERIC },                               NF_FOLD, NULL },
    { "ge",     n_ge,     V_BOOL,  2, 2,  { A_NUMERIC, A_NUMERIC },                               NF_FOLD, NULL },
    { "eq",     n_eq,     V_BOOL,  2, 2,  { A_ANY, A_ANY },                                       NF_FOLD, bind_eq },
    { "ne",     n_ne,     V_BOOL,  2, 2,  { A_ANY, A_ANY },                                       NF_FOLD, bind_eq },
    { "+",      n_plus,   V_INT,   2, 10, { A_NUMERIC, A_NUMERIC, A_NUMERIC, A_NUMERIC, A_NUMERIC,
                                           A_NUMERIC, A_NUMERIC, A_NUMERIC, A_NUMERIC, A_NUMERIC },       NF_FOLD, bind_arith },
    { "-",      n_minus,  V_INT,   1, 2,  { A_NUMERIC, A_NUMERIC },                               NF_FOLD, bind_arith },
    { "*",      n_times,  V_INT,   2, 10, { A_NUMERIC, A_NUMERIC, A_NUMERIC, A_NUMERIC, A_NUMERIC,
                                           A_NUMERIC, A_NUMERIC, A_NUMERIC, A_NUMERIC, A_NUMERIC },       NF_FOLD, bind_arith },
    { "/",      n_div,    V_INT,   2, 2,  { A_NUMERIC, A_NUMERIC },                               NF_FOLD, bind_arith },
    { "mod",    n_mod,    V_INT,   2, 2,  { A_NUMERIC, A_NUMERIC },                               NF_FOLD, bind_arith },
    { "abs",    n_abs,    V_INT,   1, 1,  { A_NUMERIC },                                          NF_FOLD, bind_arith },
    { "min",    n_min,    V_INT,   2, 10, { A_NUMERIC, A_NUMERIC, A_NUMERIC, A_NUMERIC, A_NUMERIC,
                                           A_NUMERIC, A_NUMERIC, A_NUMERIC, A_NUMERIC, A_NUMERIC },       NF_FOLD, bind_arith },
    { "max",    n_max,    V_INT,   2, 10, { A_NUMERIC, A_NUMERIC, A_NUMERIC, A_NUMERIC, A_NUMERIC,
                                           A_NUMERIC, A_NUMERIC, A_NUMERIC, A_NUMERIC, A_NUMERIC },       NF_FOLD, bind_arith },
    { "clamp",  n_clamp,  V_INT,   3, 3,  { A_NUMERIC, A_NUMERIC, A_NUMERIC },                    NF_FOLD, bind_arith },
    { "floor",  n_floor,  V_INT,   1, 1,  { A_NUMERIC },                                          NF_FOLD, NULL },
    { "round",  n_round,  V_INT,   1, 1,  { A_NUMERIC },                                          NF_FOLD, NULL },
    { "sin",    n_sin,    V_FIX,   1, 1,  { A_NUMERIC },                                          NF_FOLD, NULL },
    { "cos",    n_cos,    V_FIX,   1, 1,  { A_NUMERIC },                                          NF_FOLD, NULL },
    { "tri",    n_tri,    V_FIX,   1, 1,  { A_NUMERIC },                                          NF_FOLD, NULL },
    { "lerp",   n_lerp,   V_INT,   3, 3,  { A_ANY, A_ANY, A_NUMERIC },                            NF_FOLD, bind_lerp },
    { "rand",   n_rand,   V_INT,   0, 2,  { A_NUMERIC, A_NUMERIC },                               0,       bind_rand },
    { "str-num",  n_str_num,  V_STR, 1, 4,  { A_NUMERIC, A_INT, A_INT, A_BOOL },                0,       bind_str_num },
    { "str-time", n_str_time, V_STR, 1, 1,  { A_STR },                                           0,       bind_str_time },
    { "str-cat",  n_str_cat,  V_STR, 2, 10, { A_ANY, A_ANY, A_ANY, A_ANY, A_ANY, A_ANY, A_ANY, A_ANY, A_ANY, A_ANY }, 0, bind_str_cat },
    { "str-sub",  n_str_sub,  V_STR, 1, 3,  { A_STR, A_INT, A_INT },                             NF_FOLD, NULL },
    { "str-len",  n_str_len,  V_INT, 1, 1,  { A_STR },                                           NF_FOLD, NULL },
    { "seg7",   n_seg7,   V_BUF, 2, 6, { A_BUF, A_ANY, A_SYM, A_INT, A_INT, A_INT },                0, bind_seg7 },
    { "seg9",   n_seg9,   V_BUF, 2, 6, { A_BUF, A_ANY, A_SYM, A_INT, A_INT, A_INT },                0, bind_seg9 },
    { "seg14",  n_seg14,  V_BUF, 2, 8, { A_BUF, A_ANY, A_SYM, A_INT, A_INT, A_INT, A_INT, A_INT },  0, bind_seg14 },
    { "time-gt",  n_time_gt,  V_BOOL, 1, 1, { A_ANY },          0, bind_time_cmp },
    { "time-ge",  n_time_ge,  V_BOOL, 1, 1, { A_ANY },          0, bind_time_cmp },
    { "time-lt",  n_time_lt,  V_BOOL, 1, 1, { A_ANY },          0, bind_time_cmp },
    { "time-le",  n_time_le,  V_BOOL, 1, 1, { A_ANY },          0, bind_time_cmp },
    { "time-eq",  n_time_eq,  V_BOOL, 1, 1, { A_ANY },          0, bind_time_cmp },
    { "time-in",  n_time_in,  V_BOOL, 2, 2, { A_ANY, A_ANY },   0, bind_time_cmp },
    { "date-gt",  n_date_gt,  V_BOOL, 1, 1, { A_STR },          0, bind_time_cmp },
    { "date-ge",  n_date_ge,  V_BOOL, 1, 1, { A_STR },          0, bind_time_cmp },
    { "date-lt",  n_date_lt,  V_BOOL, 1, 1, { A_STR },          0, bind_time_cmp },
    { "date-le",  n_date_le,  V_BOOL, 1, 1, { A_STR },          0, bind_time_cmp },
    { "date-eq",  n_date_eq,  V_BOOL, 1, 1, { A_STR },          0, bind_time_cmp },
    { "date-in",  n_date_in,  V_BOOL, 2, 2, { A_STR, A_STR },   0, bind_time_cmp },
    { "env",    n_env,    V_INT, 1, 1, { A_SYM },  0, bind_env },
    { "intensity",     n_intensity,     V_FIX, 0, 1, { A_INT },                     NF_AUTOMATION, NULL },
    { "set-intensity", n_set_intensity, V_NIL, 2, 3, { A_INT, A_UNIT, A_NUMERIC },  NF_AUTOMATION | NF_OPT0, NULL },
    { "play",          n_play,          V_NIL, 2, 2, { A_INT, A_SYM },              NF_AUTOMATION, bind_play },
    { "stop",          n_stop,          V_NIL, 1, 1, { A_INT },                     NF_AUTOMATION, NULL },
    { "io",            n_io,            V_FIX, 1, 1, { A_INT },                     NF_AUTOMATION | NF_IDARG, bind_io },
    { "io-set",        n_io_set,        V_NIL, 2, 2, { A_INT, A_ANY },              NF_AUTOMATION | NF_IDARG, bind_io },
    { "in",            n_in,            V_BOOL, 1, 1, { A_INT },                    NF_IDARG, bind_in },
    { "analog",        n_analog,        V_FIX, 1, 2, { A_INT, A_INT },              NF_IDARG, bind_analog },
};

int dsl_native_count(void) { return (int)(sizeof k_natives / sizeof k_natives[0]); }
const native_def_t *dsl_native_at(int i) { return (i >= 0 && i < dsl_native_count()) ? &k_natives[i] : NULL; }

const native_def_t *dsl_find_native(const char *name)
{
    for (size_t i = 0; i < sizeof k_natives / sizeof k_natives[0]; i++)
        if (!strcmp(k_natives[i].name, name)) return &k_natives[i];
    return NULL;
}
