#define _POSIX_C_SOURCE 200809L    /* setenv, tzset */
#include "dsl.h"
#include "ws_parallel.h"
#include "gamma.h"
#include "dither.h"
#include "wire.h"
#include "fader.h"
#include "astro.h"
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "astro_cases.h"
#define LIT_AT 43200    /* the almanac's illumination is for 12:00 UTC */

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static env_t env = { .intensity = 256 };
static globals_t globals;

static void with_channel(void (*fn)(channel_t *))
{
    channel_t ch;
    if (channel_init(&ch, 10, 512, &env, &globals)) { puts("alloc failed"); g_fail++; return; }
    fn(&ch);
    channel_free(&ch);
}
static int run(channel_t *ch, const char *src, char *err)
{
    int rc = channel_set_source(ch, src, err, DSL_ERR_LEN);
    if (rc == 0) channel_render(ch);
    return rc;
}

static void t_fill(channel_t *ch)
{
    char e[DSL_ERR_LEN];
    CHECK(run(ch, "(fill (buf O) #FF0000)", e) == 0);
    for (int i = 0; i < 10; i++) CHECK(ch->buf[BUF_O][i] == 0x00FF0000);
    CHECK(run(ch, "(fill (buf O) #000000) (fill (buf O) #00FF00 2 7)", e) == 0);   /* [2,7): indices 2..6 */
    for (int i = 0; i < 10; i++) CHECK(ch->buf[BUF_O][i] == ((i >= 2 && i <= 6) ? 0x0000FF00u : 0u));
    CHECK(run(ch, "(fill (buf O) #00000000) (fill (buf O) #11223344)", e) == 0);   /* RRGGBBWW */
    CHECK(ch->buf[BUF_O][0] == 0x44112233u);
    CHECK(run(ch, "(fill (buf O) #000000) (fill (buf O) #0000FF 20 20)", e) == 0); /* out-of-range -> empty */
    CHECK(ch->buf[BUF_O][0] == 0);
    CHECK(run(ch, "(fill (buf O) #0000FF -5)", e) == 0);                           /* negative counts from end */
    for (int i = 0; i < 10; i++) CHECK(ch->buf[BUF_O][i] == (i >= 5 ? 0x0000FFu : 0u));
    CHECK(run(ch, "(fill (buf O) #000000) (fill (buf O) #0000FF -50 2)", e) == 0); /* clamps after counting back */
    for (int i = 0; i < 10; i++) CHECK(ch->buf[BUF_O][i] == (i < 2 ? 0x0000FFu : 0u));
}

static void t_scale_and_persistence(channel_t *ch)
{
    char e[DSL_ERR_LEN];
    CHECK(run(ch, "(fill (buf O) #FF8040)", e) == 0);
    CHECK(run(ch, "(scale (buf O) 0.5)", e) == 0);              /* program changed; O must persist; rounds down */
    CHECK(ch->buf[BUF_O][3] == 0x007F4020);
    channel_render(ch);                                         /* same program again: halves again */
    CHECK(ch->buf[BUF_O][3] == 0x003F2010);
    CHECK(run(ch, "(fill (buf O) #FFFFFF) (scale (buf O) 1)", e) == 0);   /* int 1 == 1.0 */
    CHECK(ch->buf[BUF_O][0] == 0x00FFFFFF);
    CHECK(run(ch, "(scale (buf O) 0)", e) == 0);
    CHECK(ch->buf[BUF_O][0] == 0);
    CHECK(run(ch, "(fill (buf O) #FFFFFF) (scale (buf O) .5 1 -1)", e) == 0);
    CHECK(ch->buf[BUF_O][0] == 0x00FFFFFF && ch->buf[BUF_O][1] == 0x007F7F7F && ch->buf[BUF_O][9] == 0x00FFFFFF);
}

static void t_nesting_globals(channel_t *ch)
{
    char e[DSL_ERR_LEN];
    CHECK(run(ch, "(scale (fill (buf X) #00FF00) 0.5) ; comment\n(fill (buf G2) #0000FF 1)", e) == 0);
    CHECK(ch->buf[BUF_X][0] == 0x00007F00);
    CHECK(globals.buf[2][0] == 0 && globals.buf[2][1] == 0x0000FF && globals.buf[2][9] == 0x0000FF);
    CHECK(run(ch, "(fill (buf g1) #010203)", e) == 0 && globals.buf[1][0] == 0x010203);
}

/* Buffer O is cleared, `src` runs, and O must match `mask` (bit i set -> pixel i == col, else 0). */
static void expect_mask(channel_t *ch, const char *src, uint32_t col, unsigned mask)
{
    char e[DSL_ERR_LEN], full[160];
    snprintf(full, sizeof full, "(fill (buf O) #000000) %s", src);
    CHECK(run(ch, full, e) == 0);
    for (int i = 0; i < 10; i++)
        if (ch->buf[BUF_O][i] != ((mask >> i & 1) ? col : 0u)) { printf("  src=%s pixel %d\n", src, i); CHECK(0); return; }
}

static void t_slice(channel_t *ch)
{
    expect_mask(ch, "(fill (slice (buf O) 2 5) #FF0000)",  0xFF0000, 0x01C);    /* [2,5) */
    expect_mask(ch, "(fill (slice (buf O) 7) #FF0000)",    0xFF0000, 0x380);    /* end defaults to len */
    expect_mask(ch, "(fill (slice (buf O) 8 20) #FF0000)", 0xFF0000, 0x300);    /* end clamps */
    expect_mask(ch, "(fill (slice (buf O) -3 2) #FF0000)", 0xFF0000, 0x000);    /* -3 is index 7: [7,2) empty */
    expect_mask(ch, "(fill (slice (buf O) 5 3) #FF0000)",  0xFF0000, 0x000);    /* empty */
    expect_mask(ch, "(fill (slice (buf O) 12) #FF0000)",   0xFF0000, 0x000);    /* past end: empty */
    expect_mask(ch, "(fill (slice (slice (buf O) 2 8) 1 3) #FF0000)", 0xFF0000, 0x018);   /* nested: [3,5) */
    expect_mask(ch, "(fill (slice (buf O) 2 8) #FF0000 1 5)",         0xFF0000, 0x078);   /* indices are slice-relative */
    CHECK(ch->perf.nodes_last == 2);                       /* both fills; constant slice folded away */

    char e[DSL_ERR_LEN];                                   /* non-constant buffer arg: slice runs every frame */
    CHECK(run(ch, "(scale (slice (fill (buf O) #FFFFFF) 0 2) 0.5)", e) == 0);
    CHECK(ch->buf[BUF_O][0] == 0x7F7F7F && ch->buf[BUF_O][1] == 0x7F7F7F && ch->buf[BUF_O][2] == 0xFFFFFF);
    CHECK(ch->perf.nodes_last == 3);

    CHECK(channel_set_source(ch, "(slice)", e, sizeof e) != 0 && strstr(e, "1 to 4 arguments"));
    CHECK(channel_set_source(ch, "(fill (slice (buf O) 1.5) #FF0000)", e, sizeof e) != 0 && strstr(e, "expected int"));
}

static void t_slice_negative_step(channel_t *ch)
{
    /* negative indices count from the end */
    expect_mask(ch, "(fill (slice (buf O) -3) #FF0000)",    0xFF0000, 0x380);   /* 7 8 9 */
    expect_mask(ch, "(fill (slice (buf O) -5 -2) #FF0000)", 0xFF0000, 0x0E0);   /* 5 6 7 */
    expect_mask(ch, "(fill (slice (buf O) 2 -1) #FF0000)",  0xFF0000, 0x1FC);   /* 2..8 */
    expect_mask(ch, "(fill (slice (buf O) -50 2) #FF0000)", 0xFF0000, 0x003);   /* clamps after wrap */

    /* step; order is checked by filling part of the slice with fill's own offsets */
    expect_mask(ch, "(fill (slice (buf O) 1 10 3) #FF0000)",       0xFF0000, 0x092);   /* 1 4 7 */
    expect_mask(ch, "(fill (slice (buf O) 0 10 2) #FF0000 1)",     0xFF0000, 0x154);   /* 0 [2 4 6 8] */
    expect_mask(ch, "(fill (slice (buf O) 2 4 5) #FF0000)",        0xFF0000, 0x004);   /* step past range: 1 pixel */
    expect_mask(ch, "(fill (slice (buf O) _ _ -1) #FF0000 0 3)",   0xFF0000, 0x380);   /* reversed: first 3 = 9 8 7 */
    expect_mask(ch, "(fill (slice (buf O) 0 10 -2) #FF0000 0 2)",  0xFF0000, 0x140);   /* 8 6 | 4 2 0: same set as +2 */
    expect_mask(ch, "(fill (slice (buf O) 1 10 -3) #FF0000 0 1)",  0xFF0000, 0x080);   /* 7 | 4 1 */
    expect_mask(ch, "(fill (slice (slice (buf O) 0 10 2) _ _ -1) #FF0000 0 1)", 0xFF0000, 0x100);  /* 8 | 6 4 2 0 */
    expect_mask(ch, "(fill (slice (slice (buf O) _ _ -1) 0 10 3) #FF0000 1)",   0xFF0000, 0x049);  /* 9 | 6 3 0 */
    expect_mask(ch, "(fill (slice (buf O) 4 4 -1) #FF0000)",       0xFF0000, 0x000);   /* empty */
    CHECK(ch->perf.nodes_last == 2);                        /* stepped constant slices still fold */

    char e[DSL_ERR_LEN];                                    /* run-time slice with negative stride */
    CHECK(run(ch, "(scale (slice (fill (buf O) #FFFFFF) _ _ -2) 0.5)", e) == 0);
    for (int i = 0; i < 10; i++) CHECK(ch->buf[BUF_O][i] == ((i % 2 == 0) ? 0x7F7F7Fu : 0xFFFFFFu));

    /* `_` as a default stand-in */
    expect_mask(ch, "(fill (buf O) #FF0000 _ 2)",  0xFF0000, 0x003);
    expect_mask(ch, "(fill (buf O) #FF0000 _ -8)", 0xFF0000, 0x003);
    expect_mask(ch, "(fill (buf O) #FF0000 -2 _)", 0xFF0000, 0x300);
    expect_mask(ch, "(fill (slice (buf O) _ _ -1) #FF0000 -2)", 0xFF0000, 0x003);   /* last 2 of reversed = 1 0 */
    expect_mask(ch, "(fill (buf O) #FF0000) (scale (buf O) 0 -3)", 0xFF0000, 0x07F); /* scale shares the convention */
    CHECK(channel_set_source(ch, "(fill (buf O) _)", e, sizeof e) != 0 && strstr(e, "'_' can only stand in"));
    CHECK(channel_set_source(ch, "(fill (slice (buf O) 0 5 0) #FF0000)", e, sizeof e) != 0 && strstr(e, "step can't be 0"));
}

static uint32_t lit_mask(const pixel_t *p, int n);
static void t_invert(channel_t *ch)
{
    char e[DSL_ERR_LEN];
    CHECK(run(ch, "(fill (buf O) #12345678) (invert (buf O))", e) == 0);          /* all four channels, W too */
    for (int i = 0; i < 10; i++) CHECK(ch->buf[BUF_O][i] == 0x87EDCBA9u);
    CHECK(run(ch, "(fill (buf O) #102030) (invert (invert (buf O)))", e) == 0);   /* returns buf; twice = unchanged */
    CHECK(ch->buf[BUF_O][0] == 0x00102030u);
    expect_mask(ch, "(invert (buf O) 2 5)", 0xFFFFFFFFu, 0x01C);                  /* range */
    expect_mask(ch, "(invert (buf O) _ 2)", 0xFFFFFFFFu, 0x003);                  /* _ */
    expect_mask(ch, "(invert (buf O) -3)", 0xFFFFFFFFu, 0x380);                   /* negative start */
    expect_mask(ch, "(invert (buf O) 4 4)", 0xFFFFFFFFu, 0x000);                  /* empty */
    expect_mask(ch, "(invert (buf O) 7 3)", 0xFFFFFFFFu, 0x000);                  /* reversed = empty */
    expect_mask(ch, "(invert (slice (buf O) 1 _ 3))", 0xFFFFFFFFu, 0x092);        /* stride 3: 1 4 7 */
    expect_mask(ch, "(invert (slice (buf O) _ _ -2) 0 2)", 0xFFFFFFFFu, 0x140);   /* reversed stride 2: 8 6 */
    CHECK(run(ch, "(seg7 (buf O) 1 abcdefg 1 1) (invert (buf O) 0 7)", e) == 0);  /* "1" = b c lit; inverse a d e f g */
    CHECK(lit_mask(ch->buf[BUF_O], 7) == 0x79 && ch->buf[BUF_O][0] == 0xFFFFFFFFu && ch->buf[BUF_O][1] == 0);
    CHECK(channel_set_source(ch, "(invert (buf O) 1 2 3)", e, sizeof e) != 0 && strstr(e, "1 to 3 arguments"));
    CHECK(channel_set_source(ch, "(invert #FF0000)", e, sizeof e) != 0 && strstr(e, "expected buffer"));
}

/* O[i] = i + 1, X[i] = 0x100 + i: distinct values so order and placement are visible. */
static void seed(channel_t *ch)
{
    for (int i = 0; i < 10; i++) { ch->buf[BUF_O][i] = (pixel_t)(i + 1); ch->buf[BUF_X][i] = (pixel_t)(0x100 + i); }
}
/* O must equal `want` (10 pixels). */
static void expect_o(channel_t *ch, const char *src, const pixel_t *want)
{
    char e[DSL_ERR_LEN];
    seed(ch);
    if (run(ch, src, e) != 0) { printf("  src=%s err=%s\n", src, e); CHECK(0); return; }
    for (int i = 0; i < 10; i++)
        if (ch->buf[BUF_O][i] != want[i]) {
            printf("  src=%s pixel %d = %lX want %lX\n", src, i, (unsigned long)ch->buf[BUF_O][i], (unsigned long)want[i]);
            CHECK(0); return;
        }
    CHECK(ch->perf.faults == 0);
}
#define X_(i) (0x100 + (i))

static void t_copy(channel_t *ch)
{
    expect_o(ch, "(copy (buf O) (buf X))",
             (pixel_t[]){ X_(0), X_(1), X_(2), X_(3), X_(4), X_(5), X_(6), X_(7), X_(8), X_(9) });
    expect_o(ch, "(copy (buf O) (buf X) 2 -2)",                       /* range applies to both */
             (pixel_t[]){ 1, 2, X_(2), X_(3), X_(4), X_(5), X_(6), X_(7), 9, 10 });
    expect_o(ch, "(copy (buf O) (slice (buf X) 0 3))",                /* shorter src: only 3 written */
             (pixel_t[]){ X_(0), X_(1), X_(2), 4, 5, 6, 7, 8, 9, 10 });
    expect_o(ch, "(copy (slice (buf O) 7) (buf X))",                  /* placement via slice */
             (pixel_t[]){ 1, 2, 3, 4, 5, 6, 7, X_(0), X_(1), X_(2) });
    expect_o(ch, "(copy (buf O) (slice (buf X) _ _ -1))",             /* reversed src */
             (pixel_t[]){ X_(9), X_(8), X_(7), X_(6), X_(5), X_(4), X_(3), X_(2), X_(1), X_(0) });

    /* overlapping views: result is as if src were read first */
    expect_o(ch, "(copy (slice (buf O) 1) (buf O))",                  /* shift right */
             (pixel_t[]){ 1, 1, 2, 3, 4, 5, 6, 7, 8, 9 });
    expect_o(ch, "(copy (buf O) (slice (buf O) 1))",                  /* shift left */
             (pixel_t[]){ 2, 3, 4, 5, 6, 7, 8, 9, 10, 10 });
    expect_o(ch, "(copy (slice (buf O) _ -1 -1) (slice (buf O) 1 _ -1))",   /* same, walked with stride -1 */
             (pixel_t[]){ 2, 3, 4, 5, 6, 7, 8, 9, 10, 10 });
    expect_o(ch, "(copy (slice (buf O) 1 _ -1) (slice (buf O) _ -1 -1))",   /* shift right, stride -1 */
             (pixel_t[]){ 1, 1, 2, 3, 4, 5, 6, 7, 8, 9 });
    expect_o(ch, "(copy (slice (buf O) 0 _ 2) (slice (buf O) 1 _ 2))",      /* odd -> even, interleaved */
             (pixel_t[]){ 2, 2, 4, 4, 6, 6, 8, 8, 10, 10 });
    size_t hw = ch->scratch.high;
    expect_o(ch, "(copy (buf O) (slice (buf O) _ _ -1))",             /* reverse in place: staged in scratch */
             (pixel_t[]){ 10, 9, 8, 7, 6, 5, 4, 3, 2, 1 });
    CHECK(ch->scratch.high >= hw && ch->scratch.high >= 10 * sizeof(pixel_t));
    expect_o(ch, "(copy (buf O) (buf O))", (pixel_t[]){ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 });
    expect_o(ch, "(copy (buf O) (buf X) 4 4)", (pixel_t[]){ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 });   /* empty */
    expect_o(ch, "(fill (copy (buf O) (buf X)) #000000 _ -2)",        /* returns dst, so it nests */
             (pixel_t[]){ 0, 0, 0, 0, 0, 0, 0, 0, X_(8), X_(9) });

    char e[DSL_ERR_LEN];
    CHECK(channel_set_source(ch, "(copy (buf O))", e, sizeof e) != 0 && strstr(e, "2 to 4 arguments"));
    CHECK(channel_set_source(ch, "(copy (buf O) #FF0000)", e, sizeof e) != 0 && strstr(e, "expected buffer"));

    channel_t tiny;                                        /* too little scratch to stage: counted fault */
    if (channel_init(&tiny, 10, 16, &env, &globals) == 0) {
        CHECK(run(&tiny, "(copy (buf O) (slice (buf O) _ _ -1))", e) == 0);
        CHECK(tiny.perf.faults == 1 && tiny.perf.last_err && strstr(tiny.perf.last_err, "scratch"));
        channel_free(&tiny);
    } else CHECK(0);
}

static void t_screen(channel_t *ch)
{
    int bad = 0;
    for (uint32_t x = 0; x <= 255 * 255; x++) if (px_div255(x) != (2 * x + 255) / 510) bad++;
    CHECK(bad == 0);
    for (uint32_t a = 0; a < 256; a++)                     /* exhaustive vs reference, all four bytes */
        for (uint32_t b = 0; b < 256; b++) {
            uint32_t want = 255 - (2 * (255 - a) * (255 - b) + 255) / 510;
            pixel_t q = px_screen(a * 0x01010101u, b * 0x01010101u);
            if (q != want * 0x01010101u || px_screen(b * 0x01010101u, a * 0x01010101u) != q) bad++;
        }
    CHECK(bad == 0);

    char e[DSL_ERR_LEN];
    CHECK(run(ch, "(fill (buf X) #FF0000) (fill (buf O) #0000FF) (screen (buf O) (buf X))", e) == 0);
    CHECK(ch->buf[BUF_O][0] == 0xFF00FF && ch->buf[BUF_X][0] == 0xFF0000);    /* src untouched */
    CHECK(run(ch, "(fill (buf X) #808080) (fill (buf O) #808080) (screen (buf O) (buf X))", e) == 0);
    CHECK(ch->buf[BUF_O][0] == 0xC0C0C0);
    CHECK(run(ch, "(fill (buf Z) #000000) (fill (buf O) #123456) (screen (buf O) (buf Z))", e) == 0);
    CHECK(ch->buf[BUF_O][5] == 0x123456);                                      /* black is neutral */
    CHECK(run(ch, "(fill (buf O) #000000) (fill (buf X) #00FF00) (screen (slice (buf O) _ _ -1) (buf X) 0 2)", e) == 0);
    for (int i = 0; i < 10; i++) CHECK(ch->buf[BUF_O][i] == (i >= 8 ? 0x00FF00u : 0u));
    CHECK(run(ch, "(fill (buf O) #404040) (screen (buf O) (buf O))", e) == 0);  /* in place with itself */
    CHECK(ch->buf[BUF_O][0] == 0x707070);                  /* 255 - round(191*191/255) = 255 - 143 = 112 */
}

static void t_blend_math(void)
{
    int bad = 0;
    for (uint32_t a = 0; a < 256; a++)
        for (uint32_t b = 0; b < 256; b++) {
            pixel_t pa = a * 0x01010101u, pb = b * 0x01010101u;   /* same value in all four bytes */
            uint32_t add = a + b > 255 ? 255 : a + b, mx = a > b ? a : b, mul = (2 * a * b + 255) / 510;
            if (px_add(pa, pb) != add * 0x01010101u) bad++;
            if (px_sub(pa, pb) != (a > b ? a - b : 0) * 0x01010101u) bad++;
            if (px_max(pa, pb) != mx * 0x01010101u) bad++;
            if (px_min(pa, pb) != (a < b ? a : b) * 0x01010101u) bad++;
            if (px_multiply(pa, pb) != mul * 0x01010101u) bad++;
            for (uint32_t k = 0; k <= UNIT_ONE; k++) {
                uint32_t m = (a * (256 - k) + b * k + 128) >> 8, t = m;
                if (k && t == a && a != b) t = b > a ? a + 1 : a - 1;    /* toward: at least one step */
                if (px_mix(pa, pb, k) != m * 0x01010101u) bad++;
                if (px_mix_toward(pa, pb, k) != t * 0x01010101u) bad++;
            }
        }
    CHECK(bad == 0);
    CHECK(px_add(0x01FF0080u, 0x01010080u) == 0x02FF00FFu);       /* bytes saturate independently */
    CHECK(px_sub(0x02FF0080u, 0x01010081u) == 0x01FE0000u);
    CHECK(px_mix(0x00FF0000u, 0x000000FFu, 0) == 0x00FF0000u && px_mix(0x00FF0000u, 0x000000FFu, 256) == 0x000000FFu);
    CHECK(px_mix_toward(0x05FA0300u, 0x00FF0304u, 26) == 0x04FB0301u);  /* bytes step independently, equal ones stay */
    bad = 0;                                                    /* a repeated fade reaches its target from either side */
    for (uint32_t k = 1; k < UNIT_ONE; k++) {
        pixel_t p = 0x00FF00FFu, q = 0xFF00FF00u;
        for (int i = 0; i < 256 * 256 && p != 0xFF00FF00u; i++) p = px_mix_toward(p, 0xFF00FF00u, k);
        for (int i = 0; i < 256 * 256 && q != 0x00FF00FFu; i++) q = px_mix_toward(q, 0x00FF00FFu, k);
        if (p != 0xFF00FF00u || q != 0x00FF00FFu) bad++;
    }
    CHECK(bad == 0);
}

static void t_blends(channel_t *ch)
{
    char e[DSL_ERR_LEN];
    struct { const char *src; pixel_t want; } cases[] = {
        { "(fill (buf O) #800000) (fill (buf X) #A00000) (add (buf O) (buf X))",      0xFF0000 },   /* saturates */
        { "(fill (buf O) #102030) (fill (buf X) #010203) (add (buf O) (buf X))",      0x112233 },
        { "(fill (buf O) #804020) (fill (buf X) #20A010) (subtract (buf O) (buf X))",      0x600010 },   /* floors at 0 */
        { "(fill (buf O) #123456) (fill (buf X) #000000) (subtract (buf O) (buf X))",      0x123456 },   /* black neutral */
        { "(fill (buf O) #804020) (fill (buf X) #20A010) (lighten (buf O) (buf X))",  0x80A020 },
        { "(fill (buf O) #804020) (fill (buf X) #20A010) (darken (buf O) (buf X))",   0x204010 },
        { "(fill (buf O) #808080) (fill (buf X) #808080) (multiply (buf O) (buf X))", 0x404040 },
        { "(fill (buf O) #123456) (fill (buf X) #FFFFFF) (multiply (buf O) (buf X))", 0x123456 },   /* white neutral */
        { "(fill (buf O) #000000) (fill (buf X) #FFFFFF) (mix (buf O) (buf X) 0.5)",  0x808080 },
        { "(fill (buf O) #123456) (fill (buf X) #FFFFFF) (mix (buf O) (buf X) 0)",    0x123456 },
        { "(fill (buf O) #123456) (fill (buf X) #ABCDEF) (mix (buf O) (buf X) 1)",    0xABCDEF },
        { "(fill (buf O) #000000) (mix (buf O) (fill (buf X) #FFFFFF) 0.25)",         0x404040 },   /* nests */
        { "(fill (buf O) #FDFF02) (fill (buf X) #FFFF00) (mix (buf O) (buf X) 0.1)",   0xFEFF01 },   /* steps toward */
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        CHECK(run(ch, cases[i].src, e) == 0);
        if (ch->buf[BUF_O][3] != cases[i].want) {
            printf("  src=%s got %06lX want %06lX\n", cases[i].src, (unsigned long)ch->buf[BUF_O][3], (unsigned long)cases[i].want);
            CHECK(0);
        }
    }
    /* mix range comes after the amount */
    expect_mask(ch, "(fill (buf X) #FFFFFF) (mix (buf O) (buf X) 1 2 4)", 0xFFFFFF, 0x00C);
    expect_mask(ch, "(fill (buf X) #FFFFFF) (mix (buf O) (buf X) 1 -2)",  0xFFFFFF, 0x300);
    /* read-modify-write op on overlapping views still sees the original src */
    expect_o(ch, "(add (slice (buf O) 1) (buf O))", (pixel_t[]){ 1, 3, 5, 7, 9, 11, 13, 15, 17, 19 });
    expect_mask(ch, "(fill (buf O) #FFFFFF) (fill (buf X) #FFFFFF) (subtract (buf O) (buf X) 2 -2)", 0xFFFFFF, 0x303);   /* range */
    expect_mask(ch, "(fill (buf O) #FFFFFF) (fill (buf X) #FFFFFF) (subtract (slice (buf O) _ _ 2) (buf X))", 0xFFFFFF, 0x2AA);

    CHECK(channel_set_source(ch, "(mix (buf O) (buf X))", e, sizeof e) != 0 && strstr(e, "3 to 5 arguments"));
    CHECK(channel_set_source(ch, "(mix (buf O) (buf X) #FF0000)", e, sizeof e) != 0 && strstr(e, "expected number"));
    CHECK(channel_set_source(ch, "(mix (buf O) (buf X) _)", e, sizeof e) != 0 && strstr(e, "'_' can only stand in"));
}

static void t_env(channel_t *ch)
{
    static const char *const names[] = { "frame", "hour", "minute", "second", "year", "month", "day", "doy", "intensity",
                                         "sunrise", "sunset", "dawn", "dusk", "moon", "moonlit" };
    env = (env_t){ .frame = 1, .hour = 2, .minute = 3, .second = 4, .year = 5, .month = 6, .day = 7, .doy = 8, .intensity = 9,
                   .sunrise = 10, .sunset = 11, .dawn = 12, .dusk = 13, .moon = 14, .moonlit = 15 };
    for (int i = 0; i < 15; i++) {                           /* name i reads i+1: fills pixels [0, i+1) */
        char src[80];
        snprintf(src, sizeof src, "(fill (buf O) #FF0000 0 (env %s))", names[i]);
        expect_mask(ch, src, 0xFF0000, (1u << (i + 1)) - 1);
    }
    CHECK(ch->perf.nodes_last == 3);                        /* clear + fill + env: env is never folded */
    env.sunrise = -1;                                       /* none today: a plain negative a program can test */
    expect_mask(ch, "(if (lt (env sunrise) 0) (fill (buf O) #FF0000 0 1))", 0xFF0000, 0x001);

    char e[DSL_ERR_LEN];                                    /* re-read every frame, no recompile */
    CHECK(run(ch, "(fill (buf O) #000000) (fill (buf O) #FF0000 0 (env second))", e) == 0);
    CHECK(ch->buf[BUF_O][3] == 0xFF0000 && ch->buf[BUF_O][4] == 0);
    env.second = 6; channel_render(ch);
    CHECK(ch->buf[BUF_O][5] == 0xFF0000 && ch->buf[BUF_O][6] == 0);

    env.frame = 0x80000003u;                                /* frame never goes negative */
    expect_mask(ch, "(fill (buf O) #FF0000 0 (env frame))", 0xFF0000, 0x007);
    expect_mask(ch, "(fill (slice (buf O) (env hour) _ (env minute)) #FF0000)", 0xFF0000, 0x124);   /* 2 5 8 */

    struct { const char *src, *want; } cases[] = {
        { "(fill (buf O) #FF0000 (env foo))",  "line 1 col 28: unknown env name" },     /* points at the name */
        { "(fill (buf O) #FF0000 (env))",      "takes 1 argument(s), got 0" },
        { "(fill (buf O) #FF0000 (env 3))",    "expected symbol, got int" },
        { "(fill (env second) #FF0000)",       "expected buffer, but 'env' returns int" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        e[0] = 0;
        CHECK(channel_set_source(ch, cases[i].src, e, sizeof e) != 0);
        if (!strstr(e, cases[i].want)) { printf("  src=%s\n  got='%s' want='%s'\n", cases[i].src, e, cases[i].want); CHECK(0); }
    }
    env = (env_t){ .intensity = 256 };
}

/* Reference gradient with plain division: pixel i of cnt over palette pal[np]. */
static pixel_t grad_ref(const pixel_t *pal, uint32_t np, uint32_t cnt, uint32_t i)
{
    if (np == 1 || cnt == 1) return pal[0];
    uint32_t num = i * (np - 1), den = cnt - 1, seg = num / den, rem = num % den;
    return rem ? px_mix(pal[seg], pal[seg + 1], (rem * 256 + den / 2) / den) : pal[seg];
}

static void t_fill_grad(channel_t *ch)
{
    char e[DSL_ERR_LEN];
    const char *pal3 = "(fill (buf G0) #FF0000 0 1) (fill (buf G0) #00FF00 1 2) (fill (buf G0) #0000FF 2 3) ";
    const pixel_t p3[] = { 0xFF0000, 0x00FF00, 0x0000FF };
    char src[400];

    snprintf(src, sizeof src, "%s(fill (buf O) #000000) (fill-grad (buf O) (slice (buf G0) 0 3))", pal3);
    CHECK(run(ch, src, e) == 0);
    for (uint32_t i = 0; i < 10; i++) CHECK(ch->buf[BUF_O][i] == grad_ref(p3, 3, 10, i));
    CHECK(ch->buf[BUF_O][0] == 0xFF0000 && ch->buf[BUF_O][9] == 0x0000FF);       /* endpoints exact */

    snprintf(src, sizeof src, "%s(fill (buf O) #000000) (fill-grad (buf O) (slice (buf G0) 0 3) 0 5)", pal3);
    CHECK(run(ch, src, e) == 0);                                                 /* 5 px: R, R/G, G, G/B, B */
    CHECK(ch->buf[BUF_O][0] == 0xFF0000 && ch->buf[BUF_O][1] == 0x808000 && ch->buf[BUF_O][2] == 0x00FF00 &&
          ch->buf[BUF_O][3] == 0x008080 && ch->buf[BUF_O][4] == 0x0000FF && ch->buf[BUF_O][5] == 0);

    snprintf(src, sizeof src, "%s(fill (buf O) #000000) (fill-grad (buf O) (slice (buf G0) 0 3 -1) 2 -2)", pal3);
    CHECK(run(ch, src, e) == 0);                                                 /* reversed palette, sub-range */
    const pixel_t p3r[] = { 0x0000FF, 0x00FF00, 0xFF0000 };
    for (uint32_t i = 0; i < 10; i++)
        CHECK(ch->buf[BUF_O][i] == ((i >= 2 && i < 8) ? grad_ref(p3r, 3, 6, i - 2) : 0u));

    /* many stops onto few pixels (stop count > pixel count): still hits both ends */
    CHECK(run(ch, "(fill-grad (buf G1) (buf G1))", e) == 0);                     /* palette == dst: identity */
    for (int i = 0; i < 10; i++) globals.buf[1][i] = (pixel_t)(i * 0x111111);
    CHECK(run(ch, "(fill (buf O) #000000) (fill-grad (buf O) (buf G1) 0 3)", e) == 0);
    CHECK(ch->buf[BUF_O][0] == 0 && ch->buf[BUF_O][2] == 9 * 0x111111u);

    expect_mask(ch, "(fill (buf G0) #00FF00) (fill-grad (buf O) (slice (buf G0) 0 1) 3 -3)", 0x00FF00, 0x078);  /* 1 color = fill */
    expect_mask(ch, "(fill (buf G0) #00FF00) (fill-grad (buf O) (slice (buf G0) 0 0))", 0x00FF00, 0x000);       /* 0 colors = no-op */
    expect_mask(ch, "(fill (buf G0) #00FF00) (fill-grad (buf O) (slice (buf G0) 0 2) 4 5)", 0x00FF00, 0x010);   /* 1 px = first color */

    /* palette inside the range being written: computed from the original palette */
    for (int i = 0; i < 10; i++) { globals.buf[1][i] = 0; }
    globals.buf[1][0] = 0xFF0000; globals.buf[1][1] = 0x0000FF;
    pixel_t want[10];
    for (uint32_t i = 0; i < 10; i++) want[i] = grad_ref((pixel_t[]){ 0xFF0000, 0x0000FF }, 2, 10, i);
    CHECK(run(ch, "(fill-grad (buf G1) (slice (buf G1) 0 2))", e) == 0);
    for (int i = 0; i < 10; i++) CHECK(globals.buf[1][i] == want[i]);

    CHECK(channel_set_source(ch, "(fill-grad (buf O) #FF0000)", e, sizeof e) != 0 && strstr(e, "expected buffer"));
}

/* Reference gradient map: BT.601 8-bit luma (77/150/29), palette spread over luma 0..255. */
static pixel_t gmap_ref(const pixel_t *pal, uint32_t np, pixel_t s)
{
    if (np == 1) return pal[0];
    uint32_t luma = (77u * PX_R(s) + 150u * PX_G(s) + 29u * PX_B(s) + 128u) >> 8;
    double pos = luma * (double)(np - 1) / 255.0;
    uint32_t seg = (uint32_t)pos;
    if (seg >= np - 1) return pal[np - 1];
    uint32_t k = (uint32_t)lround((pos - seg) * 256.0);
    return px_mix(pal[seg], pal[seg + 1], k);
}

static void t_grad_map(channel_t *ch)
{
    char e[DSL_ERR_LEN];
    const char *pal3 = "(set (buf G0) 0 #000000 #FF0000 #FFFF00) ";
    const pixel_t p3[] = { 0x000000, 0xFF0000, 0xFFFF00 };
    char src[400];

    /* grey ramp in X: luma == grey level */
    for (int i = 0; i < 10; i++) ch->buf[BUF_X][i] = (pixel_t)(i * 28) * 0x010101u;
    snprintf(src, sizeof src, "%s(grad-map (buf O) (buf X) (slice (buf G0) 0 3))", pal3);
    CHECK(run(ch, src, e) == 0);
    for (int i = 0; i < 10; i++) CHECK(ch->buf[BUF_O][i] == gmap_ref(p3, 3, ch->buf[BUF_X][i]));
    CHECK(ch->buf[BUF_O][0] == 0);                                              /* luma 0 -> first color */

    /* endpoints and luma weights: white -> last, pure colours by weight, W byte ignored */
    ch->buf[BUF_X][0] = 0xFFFFFF; ch->buf[BUF_X][1] = 0xFF000000; ch->buf[BUF_X][2] = 0x00FF00; ch->buf[BUF_X][3] = 0x0000FF;
    ch->buf[BUF_X][4] = 0xFF0000; ch->buf[BUF_X][5] = 0x808080;
    snprintf(src, sizeof src, "%s(grad-map (buf O) (buf X) (slice (buf G0) 0 3))", pal3);
    CHECK(run(ch, src, e) == 0);
    CHECK(ch->buf[BUF_O][0] == 0xFFFF00 && ch->buf[BUF_O][1] == 0);
    for (int i = 0; i < 10; i++) CHECK(ch->buf[BUF_O][i] == gmap_ref(p3, 3, ch->buf[BUF_X][i]));
    CHECK(ch->buf[BUF_O][2] != ch->buf[BUF_O][3]);                               /* green is brighter than blue */

    /* exhaustive over grey levels, 2..5 stops vs the reference (rounding of the blend may differ by at most 1) */
    for (uint32_t np = 1; np <= 5; np++) {
        pixel_t pal[5];
        for (uint32_t j = 0; j < np; j++) pal[j] = globals.buf[1][j] = (pixel_t)(0x123456u * (j + 1)) & 0xFFFFFF;
        for (int base = 0; base < 256; base += 10) {
            for (int i = 0; i < 10; i++) ch->buf[BUF_X][i] = (pixel_t)((base + i) & 0xFF) * 0x010101u;
            snprintf(src, sizeof src, "(grad-map (buf O) (buf X) (slice (buf G1) 0 %u))", np);
            CHECK(run(ch, src, e) == 0);
            for (int i = 0; i < 10; i++) {
                pixel_t got = ch->buf[BUF_O][i], want = gmap_ref(pal, np, ch->buf[BUF_X][i]);
                for (unsigned sh = 0; sh < 24; sh += 8) {
                    int dg = (int)((got >> sh) & 0xFF) - (int)((want >> sh) & 0xFF);
                    if (dg < -1 || dg > 1) { printf("  np=%u grey=%d got %06x want %06x\n", np, (base + i) & 0xFF, got, want); CHECK(0); }
                }
            }
        }
    }

    /* range, negative indices, _ */
    for (int i = 0; i < 10; i++) ch->buf[BUF_X][i] = 0xFFFFFF;
    snprintf(src, sizeof src, "%s(fill (buf O) #0000FF) (grad-map (buf O) (buf X) (slice (buf G0) 0 3) 2 -3)", pal3);
    CHECK(run(ch, src, e) == 0);
    for (int i = 0; i < 10; i++) CHECK(ch->buf[BUF_O][i] == ((i >= 2 && i < 7) ? 0xFFFF00u : 0x0000FFu));
    snprintf(src, sizeof src, "%s(fill (buf O) #0000FF) (grad-map (buf O) (buf X) (slice (buf G0) 0 3) _ 2)", pal3);
    CHECK(run(ch, src, e) == 0);
    CHECK(ch->buf[BUF_O][1] == 0xFFFF00 && ch->buf[BUF_O][2] == 0x0000FF);
    snprintf(src, sizeof src, "%s(fill (buf O) #0000FF) (grad-map (buf O) (buf X) (slice (buf G0) 0 3) 5 5)", pal3);
    CHECK(run(ch, src, e) == 0 && ch->buf[BUF_O][5] == 0x0000FF);                /* empty range: no-op */

    /* in place, strided and reversed views; shorter src limits the range */
    for (int i = 0; i < 10; i++) ch->buf[BUF_O][i] = (pixel_t)(i * 28) * 0x010101u;
    snprintf(src, sizeof src, "%s(grad-map (buf O) (buf O) (slice (buf G0) 0 3))", pal3);
    CHECK(run(ch, src, e) == 0);
    for (int i = 0; i < 10; i++) CHECK(ch->buf[BUF_O][i] == gmap_ref(p3, 3, (pixel_t)(i * 28) * 0x010101u));
    for (int i = 0; i < 10; i++) ch->buf[BUF_O][i] = (pixel_t)(i * 28) * 0x010101u;
    snprintf(src, sizeof src, "%s(grad-map (buf O) (slice (buf O) _ _ -1) (slice (buf G0) 0 3))", pal3);
    CHECK(run(ch, src, e) == 0);                                                  /* reversed onto itself: staged */
    for (int i = 0; i < 10; i++) CHECK(ch->buf[BUF_O][i] == gmap_ref(p3, 3, (pixel_t)((9 - i) * 28) * 0x010101u));
    for (int i = 0; i < 10; i++) ch->buf[BUF_X][i] = 0xFFFFFF;
    snprintf(src, sizeof src, "%s(fill (buf O) #0000FF) (grad-map (slice (buf O) 0 _ 2) (slice (buf X) 0 3) (slice (buf G0) 0 3))", pal3);
    CHECK(run(ch, src, e) == 0);
    for (int i = 0; i < 10; i++) CHECK(ch->buf[BUF_O][i] == ((i % 2 == 0 && i < 6) ? 0xFFFF00u : 0x0000FFu));

    /* palette sizes: 1 = fill where src is, 0 = no-op; palette inside dst is read before writing */
    expect_mask(ch, "(fill (buf G0) #00FF00) (grad-map (buf O) (buf X) (slice (buf G0) 0 1) 0 4)", 0x00FF00, 0x00F);
    expect_mask(ch, "(fill (buf G0) #00FF00) (grad-map (buf O) (buf X) (slice (buf G0) 0 0))", 0x00FF00, 0x000);
    for (int i = 0; i < 10; i++) { globals.buf[1][i] = (pixel_t)(i * 28) * 0x010101u; }
    pixel_t gp[2] = { globals.buf[1][3], globals.buf[1][9] }, want[10];
    for (int i = 0; i < 10; i++) want[i] = gmap_ref(gp, 2, globals.buf[1][i]);
    CHECK(run(ch, "(grad-map (buf G1) (buf G1) (set (slice (buf X) 0 2) 0 (get (buf G1) 3) (get (buf G1) 9)))", e) == 0);
    for (int i = 0; i < 10; i++) CHECK(globals.buf[1][i] == want[i]);
    for (int i = 0; i < 10; i++) { globals.buf[1][i] = (pixel_t)(i * 28) * 0x010101u; }
    for (int i = 0; i < 10; i++) want[i] = gmap_ref((pixel_t[]){ 0, 28 * 0x010101u }, 2, globals.buf[1][i]);
    CHECK(run(ch, "(grad-map (buf G1) (buf G1) (slice (buf G1) 0 2))", e) == 0);  /* palette is part of dst */
    for (int i = 0; i < 10; i++) CHECK(globals.buf[1][i] == want[i]);

    CHECK(channel_set_source(ch, "(grad-map (buf O) (buf X) #FF0000)", e, sizeof e) != 0 && strstr(e, "expected buffer"));
    CHECK(channel_set_source(ch, "(grad-map (buf O) (buf X))", e, sizeof e) != 0);
}

static void t_fill_random(channel_t *ch)
{
    char e[DSL_ERR_LEN];
    /* fill-rand: every byte of #FFFFFF scaled into [min, max]; 0.25..0.5 -> Q8 64..128 -> bytes 64..128 */
    CHECK(run(ch, "(fill-rand (buf O) #FFFFFF 0.25 0.5)", e) == 0);
    uint32_t lo = 255, hi = 0; int bad = 0;
    for (int f = 0; f < 200; f++) {
        channel_render(ch);
        for (int i = 0; i < 10; i++) {
            pixel_t p = ch->buf[BUF_O][i];
            uint32_t r = PX_R(p);
            if (PX_G(p) != r || PX_B(p) != r || r < 64 || r > 128) bad++;
            if (r < lo) lo = r;
            if (r > hi) hi = r;
        }
    }
    CHECK(bad == 0 && lo == 64 && hi == 128);               /* both ends reachable, nothing outside */
    CHECK(run(ch, "(fill-rand (buf O) #FFFFFF 0.5 0.25)", e) == 0);   /* min > max: swapped */
    for (int i = 0; i < 10; i++) CHECK(PX_R(ch->buf[BUF_O][i]) >= 64 && PX_R(ch->buf[BUF_O][i]) <= 128);
    CHECK(run(ch, "(fill-rand (buf O) #FF8040 1 1)", e) == 0);
    CHECK(ch->buf[BUF_O][0] == 0xFF8040 && ch->buf[BUF_O][9] == 0xFF8040);
    expect_mask(ch, "(fill-rand (buf O) #FF0000 1 1 2 4)", 0xFF0000, 0x00C);

    /* fill-pick */
    CHECK(run(ch, "(fill-pick (buf O) #FF0000 #0000FF 0)", e) == 0);
    for (int i = 0; i < 10; i++) CHECK(ch->buf[BUF_O][i] == 0xFF0000);
    CHECK(run(ch, "(fill-pick (buf O) #FF0000 #0000FF 1)", e) == 0);
    for (int i = 0; i < 10; i++) CHECK(ch->buf[BUF_O][i] == 0x0000FF);
    CHECK(run(ch, "(fill-pick (buf O) #FF0000 #0000FF 0.25)", e) == 0);
    int second = 0; bad = 0;
    for (int f = 0; f < 400; f++) {
        channel_render(ch);
        for (int i = 0; i < 10; i++) {
            if (ch->buf[BUF_O][i] == 0x0000FF) second++;
            else if (ch->buf[BUF_O][i] != 0xFF0000) bad++;
        }
    }
    CHECK(bad == 0 && second > 850 && second < 1150);      /* expect 1000 of 4000 */
    expect_mask(ch, "(fill-pick (buf O) #FF0000 #FF0000 0.5 -3)", 0xFF0000, 0x380);

    CHECK(channel_set_source(ch, "(fill-pick (buf O) #FF0000 #0000FF)", e, sizeof e) != 0 && strstr(e, "4 to 6 arguments"));
    CHECK(channel_set_source(ch, "(fill-rand (buf O) #FF0000 0.5)", e, sizeof e) != 0 && strstr(e, "4 to 6 arguments"));
}

static void t_set_get(channel_t *ch)
{
    char e[DSL_ERR_LEN];
    expect_mask(ch, "(set (buf O) 0 #FF0000)",                    0xFF0000, 0x001);
    expect_mask(ch, "(set (buf O) 2 #FF0000 #FF0000 #FF0000)",    0xFF0000, 0x01C);
    expect_mask(ch, "(set (buf O) -2 #FF0000 #FF0000 #FF0000)",   0xFF0000, 0x300);   /* clips at the end */
    expect_mask(ch, "(set (buf O) 20 #FF0000)",                   0xFF0000, 0x000);   /* fully outside: no-op */
    expect_mask(ch, "(set (buf O) -12 #FF0000 #FF0000 #FF0000)",  0xFF0000, 0x001);   /* -2 -1 skipped, not clamped */
    expect_mask(ch, "(set (slice (buf O) _ _ -1) 0 #FF0000)",     0xFF0000, 0x200);   /* index is view-relative */
    expect_mask(ch, "(set (slice (buf O) 0 _ 2) 1 #FF0000 #FF0000)", 0xFF0000, 0x014);
    env.second = 4;
    expect_mask(ch, "(set (buf O) (env second) #FF0000)",         0xFF0000, 0x010);   /* moving dot */
    env = (env_t){ .intensity = 256 };

    CHECK(run(ch, "(fill (buf O) #000000) (set (buf O) 0 #000001 #000002 #000003 #000004 #000005 #000006 #000007 #000008)", e) == 0);
    for (int i = 0; i < 10; i++) CHECK(ch->buf[BUF_O][i] == (i < 8 ? (pixel_t)(i + 1) : 0u));

    /* set returns its buffer, so a palette can be written and used in one expression */
    CHECK(run(ch, "(fill-grad (buf O) (set (slice (buf G0) 0 2) 0 #FF0000 #0000FF))", e) == 0);
    for (uint32_t i = 0; i < 10; i++) CHECK(ch->buf[BUF_O][i] == grad_ref((pixel_t[]){ 0xFF0000, 0x0000FF }, 2, 10, i));

    /* get */
    expect_o(ch, "(fill (buf O) (get (buf X) 3))",  (pixel_t[]){ X_(3), X_(3), X_(3), X_(3), X_(3), X_(3), X_(3), X_(3), X_(3), X_(3) });
    expect_o(ch, "(set (buf O) 0 (get (buf X) -1) (get (buf X) 0))",   /* -1 = last; values read before the write */
             (pixel_t[]){ X_(9), X_(0), 3, 4, 5, 6, 7, 8, 9, 10 });
    expect_o(ch, "(set (buf O) 0 (get (buf X) 10) (get (buf X) -11))", /* outside: black */
             (pixel_t[]){ 0, 0, 3, 4, 5, 6, 7, 8, 9, 10 });
    expect_o(ch, "(set (buf O) 0 (get (slice (buf X) _ _ -1) 1))",
             (pixel_t[]){ X_(8), 2, 3, 4, 5, 6, 7, 8, 9, 10 });
    expect_o(ch, "(set (buf O) 9 (get (buf O) 0))",                   /* copy one pixel within a buffer */
             (pixel_t[]){ 1, 2, 3, 4, 5, 6, 7, 8, 9, 1 });

    struct { const char *src, *want; } cases[] = {
        { "(set (buf O) 0)",                                                     "3 to 10 arguments, got 2" },
        { "(set (buf O) 0 #1 #2 #3 #4 #5 #6 #7 #8 #9)",                          "bad color" },
        { "(set (buf O) 0 #000001 #000002 #000003 #000004 #000005 #000006 #000007 #000008 #000009)", "3 to 10 arguments, got 11" },
        { "(set (buf O) #FF0000 #00FF00)",                                       "col 14: expected int, got color" },   /* forgot the index */
        { "(get (buf O))",                                                       "takes 2 argument(s), got 1" },
        { "(fill (get (buf O) 0) #FF0000)",                                      "expected buffer, but 'get' returns color" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        e[0] = 0;
        CHECK(channel_set_source(ch, cases[i].src, e, sizeof e) != 0);
        if (!strstr(e, cases[i].want)) { printf("  src=%s\n  got='%s' want='%s'\n", cases[i].src, e, cases[i].want); CHECK(0); }
    }
}

/* Reference: src rotated right by whole + q8/256, computed out of place. */
static void rot_ref(const pixel_t *src, pixel_t *dst, int len, int whole, uint32_t q8)
{
    for (int i = 0; i < len; i++) {
        int a = ((i - whole) % len + len) % len, b = ((i - whole - 1) % len + len) % len;
        dst[i] = q8 ? px_mix(src[a], src[b], q8) : src[a];
    }
}

/* Seed O with a blue ramp, run `src`, compare O to rot_ref over [s, s+len). */
static void expect_rot(channel_t *ch, const char *src, int s, int len, int whole, uint32_t q8)
{
    char e[DSL_ERR_LEN];
    pixel_t orig[10], want[10];
    for (int i = 0; i < 10; i++) orig[i] = ch->buf[BUF_O][i] = (pixel_t)(i * 25);
    memcpy(want, orig, sizeof want);
    rot_ref(orig + s, want + s, len, whole, q8);
    if (run(ch, src, e) != 0) { printf("  src=%s err=%s\n", src, e); CHECK(0); return; }
    for (int i = 0; i < 10; i++)
        if (ch->buf[BUF_O][i] != want[i]) {
            printf("  src=%s pixel %d = %lX want %lX\n", src, i, (unsigned long)ch->buf[BUF_O][i], (unsigned long)want[i]);
            CHECK(0); return;
        }
    CHECK(1);
}

static void t_rotate(channel_t *ch)
{
    /* whole pixels */
    expect_o(ch, "(rotate (buf O) 1)",   (pixel_t[]){ 10, 1, 2, 3, 4, 5, 6, 7, 8, 9 });       /* right */
    expect_o(ch, "(rotate (buf O) -1)",  (pixel_t[]){ 2, 3, 4, 5, 6, 7, 8, 9, 10, 1 });       /* left */
    expect_o(ch, "(rotate (buf O) 3)",   (pixel_t[]){ 8, 9, 10, 1, 2, 3, 4, 5, 6, 7 });
    expect_o(ch, "(rotate (buf O) 13)",  (pixel_t[]){ 8, 9, 10, 1, 2, 3, 4, 5, 6, 7 });       /* wraps mod len */
    expect_o(ch, "(rotate (buf O) -13)", (pixel_t[]){ 4, 5, 6, 7, 8, 9, 10, 1, 2, 3 });
    expect_o(ch, "(rotate (buf O) 10)",  (pixel_t[]){ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 });
    expect_o(ch, "(rotate (buf O) 0)",   (pixel_t[]){ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 });
    expect_o(ch, "(rotate (buf O) 1 2 5)", (pixel_t[]){ 1, 2, 5, 3, 4, 6, 7, 8, 9, 10 });     /* only [2,5) */
    expect_o(ch, "(rotate (buf O) 1 -3)",  (pixel_t[]){ 1, 2, 3, 4, 5, 6, 7, 10, 8, 9 });
    expect_o(ch, "(rotate (slice (buf O) _ _ -1) 1)", (pixel_t[]){ 2, 3, 4, 5, 6, 7, 8, 9, 10, 1 });  /* reversed view */
    expect_o(ch, "(rotate (slice (buf O) 0 _ 2) 1)",  (pixel_t[]){ 9, 2, 1, 4, 3, 6, 5, 8, 7, 10 });  /* evens only */
    expect_o(ch, "(rotate (buf O) 1 4 5)", (pixel_t[]){ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 });    /* 1 px: no-op */

    /* within 0.01 of an integer: whole-pixel rotate, no blending */
    expect_o(ch, "(rotate (buf O) 1.005)", (pixel_t[]){ 10, 1, 2, 3, 4, 5, 6, 7, 8, 9 });
    expect_o(ch, "(rotate (buf O) 0.995)", (pixel_t[]){ 10, 1, 2, 3, 4, 5, 6, 7, 8, 9 });
    expect_o(ch, "(rotate (buf O) 1.0)",   (pixel_t[]){ 10, 1, 2, 3, 4, 5, 6, 7, 8, 9 });
    expect_o(ch, "(rotate (buf O) -0.995)", (pixel_t[]){ 2, 3, 4, 5, 6, 7, 8, 9, 10, 1 });
    expect_o(ch, "(rotate (buf O) 0.009)", (pixel_t[]){ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 });

    /* sub-pixel, against the out-of-place reference */
    expect_rot(ch, "(rotate (buf O) 0.5)",   0, 10, 0, 128);
    expect_rot(ch, "(rotate (buf O) 0.25)",  0, 10, 0, 64);
    expect_rot(ch, "(rotate (buf O) 1.25)",  0, 10, 1, 64);
    expect_rot(ch, "(rotate (buf O) -0.25)", 0, 10, -1, 192);     /* = left 1, right 0.75 */
    expect_rot(ch, "(rotate (buf O) -2.5)",  0, 10, -3, 128);
    expect_rot(ch, "(rotate (buf O) 12.02)", 0, 10, 12, 5);       /* 0.02 is past the snap window */
    expect_rot(ch, "(rotate (buf O) 0.5 3 8)", 3, 5, 0, 128);

    env.second = 3;                                               /* ints from env are numbers too */
    expect_o(ch, "(rotate (buf O) (env second))", (pixel_t[]){ 8, 9, 10, 1, 2, 3, 4, 5, 6, 7 });
    env = (env_t){ .intensity = 256 };

    char e[DSL_ERR_LEN];
    CHECK(channel_set_source(ch, "(rotate (buf O))", e, sizeof e) != 0 && strstr(e, "2 to 4 arguments"));
    CHECK(channel_set_source(ch, "(rotate (buf O) #FF0000)", e, sizeof e) != 0 && strstr(e, "expected number, got color"));
    CHECK(channel_set_source(ch, "(rotate (buf O) _)", e, sizeof e) != 0 && strstr(e, "'_' can only stand in"));
}

static void t_logic(channel_t *ch)
{
    char e[DSL_ERR_LEN];
    /* comparisons, constant-folded: (fill (buf O) <c> 0 N) lights N pixels, so use if to pick N */
    struct { const char *cond; bool want; } cmp[] = {
        { "(lt 1 2)", true },   { "(lt 2 1)", false },  { "(lt 2 2)", false },
        { "(gt 2 1)", true },   { "(gt 1 2)", false },  { "(le 2 2)", true },  { "(ge 1 2)", false },
        { "(lt 1 1.5)", true }, { "(gt 1.5 1)", true }, { "(eq 2 2.0)", true }, { "(eq 2 2.001)", false },
        { "(lt -0.5 0)", true }, { "(ne 3 4)", true },
        { "(eq #FF0000 #FF0000)", true }, { "(eq #FF0000 #FF0001)", false },
        { "(eq t t)", true },   { "(eq t nil)", false }, { "(not nil)", true }, { "(not t)", false },
        { "(eq (buf O) (buf O))", true }, { "(eq (buf O) (buf X))", false },
        { "(eq (slice (buf O) 0 5) (slice (buf O) 0 5))", true }, { "(eq (slice (buf O) 0 5) (buf O))", false },
        { "(and t t)", true },  { "(and t nil)", false }, { "(and t t t t t t t t t t)", true },
        { "(or nil nil)", false }, { "(or nil t)", true }, { "(or nil nil nil nil nil nil nil nil nil t)", true },
        { "(not (and (lt 1 2) (gt 1 2)))", true },
    };
    for (size_t i = 0; i < sizeof cmp / sizeof cmp[0]; i++) {
        char src[160];
        snprintf(src, sizeof src, "(if %s (fill (buf O) #FF0000) (fill (buf O) #0000FF))", cmp[i].cond);
        e[0] = 0;
        if (run(ch, src, e) != 0) { printf("  src=%s err=%s\n", src, e); CHECK(0); continue; }
        if (ch->buf[BUF_O][0] != (cmp[i].want ? 0xFF0000u : 0x0000FFu)) { printf("  cond %s wrong\n", cmp[i].cond); CHECK(0); }
    }

    /* only the chosen branch runs (side effects show it), and no else = no-op when false */
    expect_mask(ch, "(if (lt 1 2) (fill (buf O) #FF0000 0 3) (fill (buf O) #FF0000 5))", 0xFF0000, 0x007);
    expect_mask(ch, "(if (gt 1 2) (fill (buf O) #FF0000 0 3) (fill (buf O) #FF0000 5))", 0xFF0000, 0x3E0);
    expect_mask(ch, "(if (gt 1 2) (fill (buf O) #FF0000))", 0xFF0000, 0x000);
    expect_mask(ch, "(if (gt 1 2) (fill (buf O) #FF0000) _)", 0xFF0000, 0x000);

    /* run-time conditions from env, re-evaluated every frame */
    env.second = 10;
    CHECK(run(ch, "(if (lt (env second) 30) (fill (buf O) #FF0000) (fill (buf O) #0000FF))", e) == 0);
    CHECK(ch->buf[BUF_O][0] == 0xFF0000);
    CHECK(ch->perf.nodes_last == 4);                       /* if + lt + env + one fill; the other fill never runs */
    env.second = 40; channel_render(ch);
    CHECK(ch->buf[BUF_O][0] == 0x0000FF);
    env.frame = 100000;                                    /* beyond Q16.16's int range: compared exactly */
    CHECK(run(ch, "(if (gt (env frame) 99999) (fill (buf O) #00FF00) (fill (buf O) #000000))", e) == 0);
    CHECK(ch->buf[BUF_O][0] == 0x00FF00);

    /* if as a value: colors, buffers, ints, int/decimal mix */
    CHECK(run(ch, "(fill (buf O) (if (gt (env second) 30) #00FF00 #FF0000))", e) == 0 && ch->buf[BUF_O][0] == 0x00FF00);
    CHECK(run(ch, "(fill (buf X) #000000) (fill (if (gt (env second) 30) (buf X) (buf Y)) #123456)", e) == 0);
    CHECK(ch->buf[BUF_X][0] == 0x123456);
    expect_mask(ch, "(fill (buf O) #FF0000 0 (if (gt (env second) 30) 4 2))", 0xFF0000, 0x00F);
    env = (env_t){ .intensity = 256 };

    /* short-circuit: the second operand would fault (step 0) if it were evaluated */
    CHECK(run(ch, "(fill (buf O) #000000) (if (and nil (eq (slice (buf O) 0 5 (env second)) (buf O))) (fill (buf O) #FF0000))", e) == 0);
    CHECK(ch->perf.faults == 0 && ch->buf[BUF_O][0] == 0);
    CHECK(run(ch, "(fill (buf O) #000000) (if (or t (eq (slice (buf O) 0 5 (env second)) (buf O))) (fill (buf O) #FF0000))", e) == 0);
    CHECK(ch->perf.faults == 0 && ch->buf[BUF_O][0] == 0xFF0000);
    CHECK(run(ch, "(if (and t (eq (slice (buf O) 0 5 (env second)) (buf O))) (fill (buf O) #FF0000))", e) == 0);
    CHECK(ch->perf.faults == 1);                           /* ...and it does fault when it is evaluated */
    ch->perf.faults = 0;

    /* mixed int/decimal branches give a decimal (rotate takes any number) */
    expect_o(ch, "(rotate (buf O) (if (lt 1 2) 1 0.5))", (pixel_t[]){ 10, 1, 2, 3, 4, 5, 6, 7, 8, 9 });

    struct { const char *src, *want; } errs[] = {
        { "(if 1 (fill (buf O) #FF0000))",                       "expected boolean, got int (compare it" },
        { "(if (env second) (fill (buf O) #FF0000))",            "expected boolean, but 'env' returns int (compare it" },
        { "(fill (buf O) (if t #FF0000 3))",                     "'if' branches must have the same type" },
        { "(fill (buf O) #FF0000 0 (if t 4 nil))",              "'if' branches must have the same type" },
        { "(fill (buf O) (if t #FF0000))",                       "'if' returns nothing" },
        { "(if t foo bar)",                                      "unknown name 'foo'" },
        { "(if (eq #FF0000 3) (fill (buf O) #FF0000))",          "can only compare two numbers, colors" },
        { "(if (eq (fill (buf O) #FF0000) 3) (fill (buf O) #FF0000))", "can only compare" },
        { "(if (lt #FF0000 3) (fill (buf O) #FF0000))",          "expected number, got color" },
        { "(if (and t) (fill (buf O) #FF0000))",                 "2 to 10 arguments" },
        { "(fill (buf O) #FF0000 0 (lt 1 2))",                   "expected int, got boolean" },          /* folded */
        { "(fill (buf O) #FF0000 0 (lt (env second) 2))",        "expected int, but 'lt' returns boolean" },
        { "(not 0)",                                             "expected boolean, got int" },
    };
    for (size_t i = 0; i < sizeof errs / sizeof errs[0]; i++) {
        e[0] = 0;
        CHECK(channel_set_source(ch, errs[i].src, e, sizeof e) != 0);
        if (!strstr(e, errs[i].want)) { printf("  src=%s\n  got='%s' want='%s'\n", errs[i].src, e, errs[i].want); CHECK(0); }
    }
}

/* Run `(if <cond> green red)`; true when the condition held. */
static bool dsl_true(channel_t *ch, const char *cond)
{
    char src[256], e[DSL_ERR_LEN];
    snprintf(src, sizeof src, "(if %s (fill (buf O) #00FF00) (fill (buf O) #FF0000))", cond);
    if (run(ch, src, e) != 0) { printf("  cond=%s err=%s\n", cond, e); return false; }
    return ch->buf[BUF_O][0] == 0x00FF00;
}

static void t_arith(channel_t *ch)
{
    struct { const char *expr, *want; } eqs[] = {
        { "(+ 1 2)", "3" }, { "(+ 1 2 3 4 5 6 7 8 9 10)", "55" }, { "(+ 1 0.5)", "1.5" }, { "(+ -1 0.25)", "-0.75" },
        { "(- 5)", "-5" }, { "(- 5 7)", "-2" }, { "(- 1.5)", "-1.5" }, { "(- 1 0.25)", "0.75" },
        { "(* 3 4)", "12" }, { "(* 2 1.5)", "3" }, { "(* 1.5 1.5)", "2.25" }, { "(* -2 0.5 3)", "-3" },
        { "(/ 7 2)", "3" }, { "(/ -7 2)", "-4" }, { "(/ 7 -2)", "-4" }, { "(/ 6 3)", "2" },          /* ints: floor */
        { "(/ 7 2.0)", "3.5" }, { "(/ 7.5 2.5)", "3" }, { "(/ -1 4.0)", "-0.25" },
        { "(mod 7 3)", "1" }, { "(mod -1 60)", "59" }, { "(mod 7 -3)", "-2" }, { "(mod 370 360)", "10" },
        { "(mod 7.5 2)", "1.5" }, { "(mod -0.5 2)", "1.5" }, { "(mod 5.25 0.5)", "0.25" },
        { "(abs -3)", "3" }, { "(abs 3)", "3" }, { "(abs -2.5)", "2.5" },
        { "(floor 2.75)", "2" }, { "(floor -2.5)", "-3" }, { "(floor 7)", "7" },
        { "(round 2.5)", "3" }, { "(round 2.4)", "2" }, { "(round -2.5)", "-2" }, { "(round -2.6)", "-3" },
        { "(min 3 1 2)", "1" }, { "(max 3 1 2)", "3" }, { "(min 2 1.5)", "1.5" }, { "(max -1 -0.5)", "-0.5" },
        { "(max 1 2 3 4 5 6 7 8 9 10)", "10" }, { "(min (env second) 5)", "0" },
        { "(clamp 5 0 3)", "3" }, { "(clamp -1 0 3)", "0" }, { "(clamp 1.5 0 3)", "1.5" }, { "(clamp 5 3 0)", "3" },
        { "(* 100000 100000)", "2147483647" }, { "(+ 2147483647 1)", "2147483647" },                /* saturate */
        { "(- -2147483647 5)", "-2147483647" },
        { "(sin 0)", "0" }, { "(sin 90)", "1" }, { "(sin 30)", "0.5" }, { "(sin 270)", "-1" }, { "(sin -90)", "-1" },
        { "(sin 450)", "1" }, { "(cos 0)", "1" }, { "(cos 180)", "-1" }, { "(cos 90)", "0" }, { "(cos -360)", "1" },
        { "(+ (* 2 (env second)) 1)", "1" },                   /* env second is 0 here */
    };
    for (size_t i = 0; i < sizeof eqs / sizeof eqs[0]; i++) {
        char cond[160];
        snprintf(cond, sizeof cond, "(eq %s %s)", eqs[i].expr, eqs[i].want);
        if (!dsl_true(ch, cond)) { printf("  %s != %s\n", eqs[i].expr, eqs[i].want); CHECK(0); }
    }
    CHECK(!dsl_true(ch, "(eq (sin 30) 0.6)") && !dsl_true(ch, "(eq (/ 7 2) 3.5)"));   /* the harness can fail */
    CHECK(dsl_true(ch, "(and (gt (* 100000 0.001) 100) (lt (* 100000 0.001) 101))"));   /* no early saturation */
    CHECK(dsl_true(ch, "(and (gt (/ 1 3.0) 0.33332) (lt (/ 1 3.0) 0.33334))"));
    CHECK(dsl_true(ch, "(gt (* 30000.0 2) 32767)"));                                     /* decimal saturates */

    /* sin/cos vs libm: every degree in -720..720, plus fractions (linear interpolation), within 5/65536 */
    int bad = 0;
    for (int d = -720; d <= 720; d++)
        for (int q = 0; q < 4; q++) {
            if (q && (d % 7)) continue;                    /* fractions on a subset, to keep the test quick */
            double deg = d + q * 0.25, rad = deg * 3.14159265358979323846 / 180;
            char cond[200];
            snprintf(cond, sizeof cond, "(and (lt (abs (- (sin %g) %.6f)) 0.00008) (lt (abs (- (cos %g) %.6f)) 0.00008))",
                     deg, sin(rad), deg, cos(rad));
            if (!dsl_true(ch, cond)) { if (bad++ < 5) printf("  sin/cos(%g) off\n", deg); }
        }
    CHECK(bad == 0);
    env.frame = 0x7FFFFFFF;                                 /* huge ints are reduced mod 360 exactly: 2147483647 % 360 = 127 */
    CHECK(dsl_true(ch, "(eq (sin (env frame)) (sin 127))"));
    env = (env_t){ .intensity = 256 };

    /* types: ints stay ints, decimals need floor/round to become indices */
    expect_mask(ch, "(fill (buf O) #FF0000 0 (+ 1 2))", 0xFF0000, 0x007);
    expect_mask(ch, "(fill (buf O) #FF0000 0 (floor (* 2.5 2)))", 0xFF0000, 0x01F);
    expect_mask(ch, "(fill (buf O) #FF0000 0 (/ (+ (env second) 9) 2))", 0xFF0000, 0x00F);   /* (0+9)/2 = 4 */
    CHECK(run(ch, "(fill (buf O) #FFFFFF) (scale (buf O) (+ 0.5 (* 0.5 (sin 90))))", (char[DSL_ERR_LEN]){0}) == 0);
    CHECK(ch->buf[BUF_O][0] == 0xFFFFFF);
    expect_o(ch, "(rotate (buf O) (* 2 0.5))", (pixel_t[]){ 10, 1, 2, 3, 4, 5, 6, 7, 8, 9 });

    char e[DSL_ERR_LEN];                                    /* division by zero: compile error if constant, fault at run time */
    CHECK(channel_set_source(ch, "(fill (buf O) #FF0000 0 (/ 1 0))", e, sizeof e) != 0 && strstr(e, "division by zero"));
    CHECK(channel_set_source(ch, "(fill (buf O) #FF0000 0 (mod 1 0))", e, sizeof e) != 0 && strstr(e, "division by zero"));
    CHECK(run(ch, "(fill (buf O) #FF0000 0 (mod 5 (env second)))", e) == 0 && ch->perf.faults == 1);
    ch->perf.faults = 0;
    struct { const char *src, *want; } errs[] = {
        { "(fill (buf O) #FF0000 0 (/ 7 2.0))",          "expected int, got number (use floor or round)" },
        { "(fill (buf O) #FF0000 0 (* (env second) 1.5))", "expected int, but '*' returns number (use floor or round)" },
        { "(fill (buf O) #FF0000 0 (+ 1))",              "2 to 10 arguments" },
        { "(fill (buf O) #FF0000 0 (+ 1 #FF0000))",      "expected number, got color" },
        { "(fill (buf O) #FF0000 0 (- 1 2 3))",          "1 to 2 arguments" },
        { "(fill (buf O) (sin 90))",                     "expected color, got number" },
        { "(fmul (buf O) 0.5)",                          "unknown function 'fmul'" },             /* renamed: scale */
        { "(sub (buf O) (buf X))",                       "unknown function 'sub'" },              /* renamed: subtract */
        { "(fill (buf O) #FF0000 0 (max 1))",            "2 to 10 arguments" },
        { "(fill (buf O) #FF0000 0 (max (buf O) (buf X)))", "expected number, got buffer" },     /* blend is lighten now */
    };
    for (size_t i = 0; i < sizeof errs / sizeof errs[0]; i++) {
        e[0] = 0;
        CHECK(channel_set_source(ch, errs[i].src, e, sizeof e) != 0);
        if (!strstr(e, errs[i].want)) { printf("  src=%s\n  got='%s' want='%s'\n", errs[i].src, e, errs[i].want); CHECK(0); }
    }
}

static void t_fill_pat(channel_t *ch)
{
    /* X[i] = 0x100+i, O[i] = i+1 (seed) */
    expect_o(ch, "(fill-pat (buf O) (slice (buf X) 0 3))",
             (pixel_t[]){ X_(0), X_(1), X_(2), X_(0), X_(1), X_(2), X_(0), X_(1), X_(2), X_(0) });
    expect_o(ch, "(fill-pat (buf O) (slice (buf X) 0 3) _ _ _ 2 -2)",      /* pattern starts at the range start */
             (pixel_t[]){ 1, 2, X_(0), X_(1), X_(2), X_(0), X_(1), X_(2), 9, 10 });
    expect_o(ch, "(fill-pat (buf O) (slice (buf X) 0 3 -1))",              /* reversed pattern */
             (pixel_t[]){ X_(2), X_(1), X_(0), X_(2), X_(1), X_(0), X_(2), X_(1), X_(0), X_(2) });
    expect_o(ch, "(fill-pat (slice (buf O) 0 _ 3) (slice (buf X) 0 2))",   /* every 3rd pixel */
             (pixel_t[]){ X_(0), 2, 3, X_(1), 5, 6, X_(0), 8, 9, X_(1) });
    expect_o(ch, "(fill-pat (buf O) (slice (buf X) 0 20) _ _ _ 0 4)",      /* pattern longer than range */
             (pixel_t[]){ X_(0), X_(1), X_(2), X_(3), 5, 6, 7, 8, 9, 10 });
    expect_o(ch, "(fill-pat (buf O) (slice (buf X) 0 0))", (pixel_t[]){ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 });   /* empty: no-op */
    expect_o(ch, "(fill-pat (buf O) (slice (buf O) 0 3) _ _ _ 3)",         /* repeat O's own first 3 */
             (pixel_t[]){ 1, 2, 3, 1, 2, 3, 1, 2, 3, 1 });
    expect_o(ch, "(fill-pat (buf O) (slice (buf O) 1 3))",                 /* pattern inside the written range */
             (pixel_t[]){ 2, 3, 2, 3, 2, 3, 2, 3, 2, 3 });
    expect_o(ch, "(scale (fill-pat (buf O) (slice (buf X) 0 1)) 1 _ 2)",   /* returns dst */
             (pixel_t[]){ X_(0), X_(0), X_(0), X_(0), X_(0), X_(0), X_(0), X_(0), X_(0), X_(0) });
    /* phase / stretch / mirror, hand-checked */
    expect_o(ch, "(fill-pat (buf O) (slice (buf X) 0 3) 1)",              /* phase +1 moves right */
             (pixel_t[]){ X_(2), X_(0), X_(1), X_(2), X_(0), X_(1), X_(2), X_(0), X_(1), X_(2) });
    expect_o(ch, "(fill-pat (buf O) (slice (buf X) 0 3) -1)",
             (pixel_t[]){ X_(1), X_(2), X_(0), X_(1), X_(2), X_(0), X_(1), X_(2), X_(0), X_(1) });
    expect_o(ch, "(fill-pat (buf O) (slice (buf X) 0 3) _ 2)",            /* AABBCC */
             (pixel_t[]){ X_(0), X_(0), X_(1), X_(1), X_(2), X_(2), X_(0), X_(0), X_(1), X_(1) });
    expect_o(ch, "(fill-pat (buf O) (slice (buf X) 0 3) _ _ t)",          /* ABCCBA */
             (pixel_t[]){ X_(0), X_(1), X_(2), X_(2), X_(1), X_(0), X_(0), X_(1), X_(2), X_(2) });
    expect_o(ch, "(fill-pat (buf O) (slice (buf X) 0 3) _ 2 t)",          /* AABBCCCCBBAA */
             (pixel_t[]){ X_(0), X_(0), X_(1), X_(1), X_(2), X_(2), X_(2), X_(2), X_(1), X_(1) });
    expect_o(ch, "(fill-pat (buf O) (slice (buf X) 0 1) _ _ t)",          /* one pixel mirrored */
             (pixel_t[]){ X_(0), X_(0), X_(0), X_(0), X_(0), X_(0), X_(0), X_(0), X_(0), X_(0) });
    expect_o(ch, "(fill-pat (buf O) (slice (buf X) 0 3) 1 2 nil 2 8)",    /* everything, in a sub-range */
             (pixel_t[]){ 1, 2, X_(2), X_(0), X_(0), X_(1), X_(1), X_(2), 9, 10 });

    /* every combination against a reference: seq = pattern (mirrored), each pixel x stretch; dst[i] = seq[(i - phase) mod T] */
    int bad = 0;
    for (int len = 1; len <= 4; len++)
        for (int st = 1; st <= 3; st++)
            for (int mi = 0; mi <= 1; mi++)
                for (int ph = -13; ph <= 13; ph++) {
                    char src[96];
                    snprintf(src, sizeof src, "(fill-pat (buf O) (slice (buf X) 0 %d) %d %d %s)", len, ph, st, mi ? "t" : "nil");
                    pixel_t want[10];
                    int T = len * (mi ? 2 : 1) * st;
                    for (int i = 0; i < 10; i++) {
                        int j = ((i - ph) % T + T) % T, q = j / st;
                        if (q >= len) q = 2 * len - 1 - q;
                        want[i] = (pixel_t)X_(q);
                    }
                    char e2[DSL_ERR_LEN];
                    seed(ch);
                    if (run(ch, src, e2) != 0 || memcmp(ch->buf[BUF_O], want, sizeof want)) { if (bad++ < 3) printf("  %s wrong\n", src); }
                }
    CHECK(bad == 0);
    env.frame = 0x7FFFFFFF;                                    /* huge phase: exact modulo */
    expect_o(ch, "(fill-pat (buf O) (slice (buf X) 0 3) (env frame))",    /* 2147483647 mod 3 = 1: like phase 1 */
             (pixel_t[]){ X_(2), X_(0), X_(1), X_(2), X_(0), X_(1), X_(2), X_(0), X_(1), X_(2) });
    env = (env_t){ .intensity = 256 };

    char e[DSL_ERR_LEN];
    CHECK(channel_set_source(ch, "(fill-pat (buf O) #FF0000)", e, sizeof e) != 0 && strstr(e, "expected buffer"));
    CHECK(channel_set_source(ch, "(fill-pat (buf O) (buf X) _ 0)", e, sizeof e) != 0 && strstr(e, "stretch must be at least 1"));
    CHECK(channel_set_source(ch, "(fill-pat (buf O) (buf X) _ _ 1)", e, sizeof e) != 0 && strstr(e, "expected boolean"));
    CHECK(run(ch, "(fill-pat (buf O) (buf X) _ (env second))", e) == 0 && ch->perf.faults == 1);   /* run time: fault */
    ch->perf.faults = 0;
}

static void t_vars(channel_t *ch)
{
    char e[DSL_ERR_LEN];
    /* slice refs */
    CHECK(run(ch, "(def p (slice (buf G0) 0 3)) (set p 0 #FF0000 #00FF00 #0000FF) (fill-grad (buf O) p)", e) == 0);
    for (uint32_t i = 0; i < 10; i++) CHECK(ch->buf[BUF_O][i] == grad_ref((pixel_t[]){ 0xFF0000, 0x00FF00, 0x0000FF }, 3, 10, i));
    /* numbers, redefinition reading the old value, colors, booleans */
    expect_mask(ch, "(def n 4) (fill (buf O) #FF0000 0 n)", 0xFF0000, 0x00F);
    expect_mask(ch, "(def n 2) (def n (+ n 3)) (fill (buf O) #FF0000 0 n)", 0xFF0000, 0x01F);
    expect_mask(ch, "(def c #00FF00) (def on (lt 1 2)) (if on (fill (buf O) c 0 2))", 0x00FF00, 0x003);
    expect_mask(ch, "(def half 0.5) (fill (buf O) #FF0000) (scale (buf O) half 0 0) (fill (buf O) #FF0000 0 (floor (* half 6)))",
                0xFF0000, 0x3FF);
    /* computed every run from env */
    env.second = 3;
    CHECK(run(ch, "(fill (buf O) #000000) (def n (* 2 (env second))) (fill (buf O) #FF0000 0 n)", e) == 0);
    CHECK(ch->buf[BUF_O][5] == 0xFF0000 && ch->buf[BUF_O][6] == 0);
    env.second = 4; channel_render(ch);
    CHECK(ch->buf[BUF_O][7] == 0xFF0000 && ch->buf[BUF_O][8] == 0);
    env = (env_t){ .intensity = 256 };
    /* a variable holding a buffer works with every buffer function */
    expect_o(ch, "(def pat (slice (buf X) 0 2)) (def dst (slice (buf O) 4)) (fill-pat dst pat)",
             (pixel_t[]){ 1, 2, 3, 4, X_(0), X_(1), X_(0), X_(1), X_(0), X_(1) });
    CHECK(ch->perf.faults == 0);

    struct { const char *src, *want; } errs[] = {
        { "(fill (buf O) #FF0000 0 n)",                         "unknown name 'n' (define it first" },
        { "(fill (buf O) #FF0000 0 (+ n 1)) (def n 2)",         "unknown name 'n'" },                   /* use before def */
        { "(def n 1) (def n 1.5)",                              "'n' is already int, it can't become number (define it as a decimal" },
        { "(def c #FF0000) (fill (buf O) #FF0000 0 c)",         "expected int, but variable 'c' is color" },
        { "(def c #FF0000) (fill c #FF0000)",                   "expected buffer, but variable 'c' is color" },
        { "(if t (def n 1))",                                   "def only works at the top level" },
        { "(def a.b 1)",                                        "bad variable name" },
        { "(def t 1)",                                          "bad variable name" },
        { "(def x)",                                            "'def' takes a name and a value" },
        { "(def x 1 2)",                                        "'def' takes a name and a value" },
        { "(def x (if t (fill (buf O) #000000)))",              "returns nothing" },
        { "(def x (env second)) (fill x #FF0000)",              "expected buffer, but variable 'x' is int" },
        { "(fill O #FF0000)",                                   "did you mean (buf O)?" },
    };
    for (size_t i = 0; i < sizeof errs / sizeof errs[0]; i++) {
        e[0] = 0;
        CHECK(channel_set_source(ch, errs[i].src, e, sizeof e) != 0);
        if (!strstr(e, errs[i].want)) { printf("  src=%s\n  got='%s' want='%s'\n", errs[i].src, e, errs[i].want); CHECK(0); }
    }
    static char many[1200];                                    /* slot limit */
    many[0] = 0;
    for (int i = 0; i <= DSL_MAX_VARS; i++) { char d[24]; snprintf(d, sizeof d, "(def v%d %d)", i, i); strcat(many, d); }
    CHECK(channel_set_source(ch, many, e, sizeof e) != 0 && strstr(e, "too many variables"));

    channel_t tiny;                                            /* slots live in the program block, not in scratch */
    if (channel_init(&tiny, 10, 16, &env, &globals) == 0) {
        CHECK(run(&tiny, "(def a 1) (def b 2) (fill (buf O) #FF0000 0 (+ a b))", e) == 0);
        CHECK(tiny.perf.faults == 0 && tiny.buf[BUF_O][2] == 0xFF0000 && tiny.buf[BUF_O][3] == 0);
        channel_free(&tiny);
    } else CHECK(0);
}

static void t_errors(channel_t *ch)
{
    char e[DSL_ERR_LEN];
    struct { const char *src, *want; } cases[] = {
        { "(blur (buf O))",              "unknown function 'blur'" },
        { "(fill (buf O))",              "2 to 4 arguments, got 1" },
        { "(scale O 0.5)",                "did you mean (buf O)" },
        { "(scale (buf O) #FF0000)",      "expected number, got color" },
        { "(fill (buf Q) #FF0000)",      "unknown buffer id" },
        { "(fill (buf O) #12345)",       "bad color" },
        { "(scale (buf O) 1.2.3)",        "bad number" },
        { "(fill (buf O) #FF0000",       "unclosed '('" },
        { "(fill (buf O) #FF0000))",     "unexpected ')'" },
        { "fill",                        "top-level forms must be calls" },
        { "()",                          "empty list" },
        { "(fill (buf O) #FF0000 1.5)",  "expected int, got number" },
        { "\n\n  (nope)",                "line 3 col 4" },
    };
    CHECK(run(ch, "(fill (buf O) #123456)", e) == 0);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        e[0] = 0;
        int rc = channel_set_source(ch, cases[i].src, e, sizeof e);
        CHECK(rc != 0);
        if (!strstr(e, cases[i].want)) { printf("  src=%s\n  got='%s' want='%s'\n", cases[i].src, e, cases[i].want); CHECK(0); }
    }
    channel_render(ch);                                       /* old program must still be running */
    CHECK(ch->buf[BUF_O][0] == 0x123456 && ch->perf.faults == 0);

    char deep[256] = "";                                       /* nesting limit */
    for (int i = 0; i < 20; i++) strcat(deep, "(scale ");
    strcat(deep, "(buf O)");
    CHECK(channel_set_source(ch, deep, e, sizeof e) != 0 && strstr(e, "nesting too deep"));

    static char big[40000];                                    /* oversize program fails cleanly */
    big[0] = 0;
    for (int i = 0; i < 1000; i++) strcat(big, "(fill (buf O) #010203 1 1)");
    CHECK(channel_set_source(ch, big, e, sizeof e) != 0 && strstr(e, "too large"));
}

static void t_swap_atomic(channel_t *ch)
{
    char e[DSL_ERR_LEN];
    CHECK(channel_set_source(ch, "(fill (buf O) #010101)", e, sizeof e) == 0);
    CHECK(channel_set_source(ch, "(fill (buf O) #020202)", e, sizeof e) == 0);   /* supersedes pending, no leak */
    channel_render(ch);
    CHECK(ch->buf[BUF_O][0] == 0x020202);
    CHECK(strstr(ch->source, "#020202") != NULL);
    CHECK(ch->perf.nodes_last == 1);                       /* (buf O) is folded at compile time: one node per run */
}

static void t_two_step_and_long(channel_t *ch)
{
    char e[DSL_ERR_LEN];
    /* compile now, publish later: nothing changes until channel_set_program */
    CHECK(run(ch, "(fill (buf O) #000001)", e) == 0);
    program_t *p = dsl_compile(ch, "(fill (buf O) #000002)", e, sizeof e);
    CHECK(p != NULL);
    channel_render(ch);
    CHECK(ch->buf[BUF_O][0] == 1 && strstr(ch->source, "#000001"));
    channel_set_program(ch, p, "(fill (buf O) #000002)");
    channel_render(ch);
    CHECK(ch->buf[BUF_O][0] == 2 && strstr(ch->source, "#000002"));

    /* a ~3900-char multi-line program with comments: temp memory is reused per top-level form */
    static char src[4000];
    src[0] = 0;
    for (int i = 0; strlen(src) < 3800; i++) {
        char line[96];
        snprintf(line, sizeof line, "; step %d: set pixel %d\n(set (buf O) %d #%06X)   ; comment\n", i, i % 10, i % 10, i);
        strcat(src, line);
    }
    CHECK(run(ch, src, e) == 0);
    CHECK(ch->perf.faults == 0 && ch->source && strlen(ch->source) > 3800);
}

/* Read lane `lane` back out of a packed buffer as a waveform and decode it: every bit must be exactly 10000 or 11100. */
static int ws_decode(const uint8_t *buf, unsigned width, unsigned lane, size_t nbytes_total, uint8_t *out, size_t nout)
{
    size_t samples = nbytes_total * 8 / width, s = 0;
    for (size_t i = 0; i < nout; i++) {
        uint8_t v = 0;
        for (int b = 0; b < 8; b++, s += WS_SAMPLES_PER_BIT) {
            if (s + WS_SAMPLES_PER_BIT > samples) return -1;
            unsigned pat = 0;
            for (unsigned k = 0; k < WS_SAMPLES_PER_BIT; k++) {
                size_t bit = (s + k) * width + lane;
                pat = pat << 1 | ((buf[bit >> 3] >> (bit & 7)) & 1u);
            }
            if (pat == 0x10) v = (uint8_t)(v << 1);                /* 10000: 300 ns high */
            else if (pat == 0x1C) v = (uint8_t)(v << 1 | 1);       /* 11100: 900 ns high */
            else return -2;
        }
        out[i] = v;
    }
    for (; s < samples; s++)                                       /* past the data: line stays low */
        if ((buf[(s * width + lane) >> 3] >> ((s * width + lane) & 7)) & 1u) return -3;
    return 0;
}

static void t_ws_parallel(void)
{
    CHECK(ws_par_bytes(50, 2) == 1500 && ws_par_bytes(1, 1) == 15);
    for (size_t px = 1; px <= 300; px++)                           /* frames are whole 32-bit words, never shorter */
        for (unsigned w = 1; w <= 8; w *= 2) {
            size_t fb = ws_par_frame_bytes(px, w);
            if (fb % 4 || fb < ws_par_bytes(px, w) || fb - ws_par_bytes(px, w) > 3) { CHECK(0); px = 301; break; }
        }
    CHECK(ws_par_frame_bytes(50, 1) == 752);                       /* the case seen on hardware: 750 -> 752 */
    CHECK(WS_SAMPLE_HZ * 72u / 1000u == 239999u);                   /* 240 MHz / 72, 300 ns per sample */
    static const unsigned widths[] = { 1, 2, 4, 8 };
    for (size_t w = 0; w < 4; w++) {
        unsigned width = widths[w];
        size_t px = 7, nb = ws_par_bytes(px, width);
        uint8_t *buf = calloc(1, nb), want[8][21], got[21];
        for (unsigned lane = 0; lane < width; lane++) {
            size_t n = 21 - 3 * (lane % 3);                        /* shorter strips on some lanes */
            for (size_t i = 0; i < n; i++) want[lane][i] = (uint8_t)(0xA5 ^ (i * 37) ^ (lane * 91));
            want[lane][0] = 0x00; want[lane][1] = 0xFF;             /* all-zero and all-one bytes */
            /* encode pixel by pixel (3 bytes at a time), as led_out does */
            for (size_t i = 0; i < n; i += 3) ws_par_encode(buf, width, lane, i, want[lane] + i, 3);
        }
        for (unsigned lane = 0; lane < width; lane++) {
            size_t n = 21 - 3 * (lane % 3);
            int r = ws_decode(buf, width, lane, nb, got, n);
            if (r || memcmp(got, want[lane], n)) { printf("  ws width %u lane %u: decode %d\n", width, lane, r); CHECK(0); }
            else CHECK(1);
        }
        free(buf);
    }
    uint8_t one[3] = { 0x12, 0x34, 0x56 }, got[3], lane1[45] = { 0 };   /* lanes don't leak into each other */
    ws_par_encode(lane1, 2, 1, 0, one, 3);
    CHECK(ws_decode(lane1, 2, 1, sizeof lane1, got, 3) == 0 && !memcmp(got, one, 3));
    CHECK(ws_decode(lane1, 2, 0, sizeof lane1, got, 3) == -2);         /* lane 0 never went high */
}

static void t_arena(void)
{
    uint8_t mem[256]; arena_t a; arena_init(&a, mem, sizeof mem);
    void *p1 = arena_alloc(&a, 100), *p2 = arena_alloc(&a, 100);
    CHECK(p1 && p2 && !arena_alloc(&a, 100) && ((uintptr_t)p1 & 7) == 0 && ((uintptr_t)p2 & 7) == 0);
    size_t hw = a.high; arena_reset(&a);
    CHECK(a.used == 0 && a.high == hw && arena_alloc(&a, 200) == (void *)a.base);
    /* Memory that is only 4-aligned (ESP heaps do that): base is rounded up, mem stays what was given, and that is
     * what must be freed. channel_free freed base and corrupted the heap (C6 no-WiFi build crashed at boot). */
    uint8_t *odd = (uint8_t *)(((uintptr_t)mem + 7) & ~(uintptr_t)7) + 4;
    arena_init(&a, odd, 64);
    CHECK(a.mem == odd && a.base == odd + 4 && a.size == 60 && ((uintptr_t)arena_alloc(&a, 8) & 7) == 0);
}

static void t_px_scale_exhaustive(void)
{
    int bad = 0;
    for (uint32_t s = 0; s <= 256; s++)
        for (uint32_t c = 0; c < 256; c++) {
            uint32_t want = (c * s + 128) >> 8;
            pixel_t p = (c << 24) | (c << 16) | (c << 8) | c, q = px_scale(p, s);
            if (PX_W(q) != want || PX_R(q) != want || PX_G(q) != want || PX_B(q) != want) bad++;
        }
    CHECK(bad == 0);
    CHECK(px_scale(0xFFFFFFFFu, 256) == 0xFFFFFFFFu && px_scale(0xFFFFFFFFu, 0) == 0);
    bad = 0;
    for (uint32_t s = 0; s <= 256; s++)
        for (uint32_t c = 0; c < 256; c++) {
            uint32_t want = (c * s) >> 8;
            pixel_t p = (c << 24) | (c << 16) | (c << 8) | c, q = px_scale_down(p, s);
            if (PX_W(q) != want || PX_R(q) != want || PX_G(q) != want || PX_B(q) != want) bad++;
        }
    CHECK(bad == 0);
    /* a repeated fade reaches black: every factor below 1 (the user's (scale (buf O) 0.9) stuck at 4 with rounding) */
    bad = 0;
    for (uint32_t s = 0; s < 256; s++) {
        pixel_t p = 0xFFFFFFFFu;
        for (int i = 0; i < 256 * 256 && p; i++) p = px_scale_down(p, s);
        if (p) bad++;
    }
    CHECK(bad == 0);
}

static void t_stats(void)
{
    stat_us_t s = {0};
    stat_record(&s, 100); stat_record(&s, 200); stat_record(&s, 50);
    CHECK(s.min == 50 && s.max == 200 && s.last == 50 && s.count == 3 && s.avg > 50 && s.avg < 200);
}

/* Gamma tables: integer-only, so compare with libm: off by at most 1, mostly exact, monotonic, ends fixed. */
static void t_gamma(void)
{
    uint8_t lut[256];
    int worst = 0, inexact = 0;
    for (unsigned g = GAMMA_MIN; g <= GAMMA_MAX; g++) {
        gamma_lut(lut, g);
        CHECK(lut[0] == 0 && lut[255] == 255);
        for (int i = 1; i < 256; i++) {
            int want = (int)lround(255.0 * pow(i / 255.0, g / 100.0)), d = abs(lut[i] - want);
            if (d > worst) worst = d;
            if (d) inexact++;
            if (lut[i] < lut[i - 1]) { printf("  gamma %u not monotonic at %d\n", g, i); CHECK(0); }
        }
    }
    CHECK(worst <= 1);
    CHECK(inexact < 201 * 255 / 1000);                    /* under 0.1% of entries differ from libm by 1 */
    gamma_lut(lut, GAMMA_OFF);
    for (int i = 0; i < 256; i++) CHECK(lut[i] == i);
    gamma_lut(lut, 220);
    CHECK(lut[128] == (int)lround(255.0 * pow(128 / 255.0, 2.2)) && lut[1] == 0 && lut[64] == 12);
    gamma_lut(lut, 999);                                   /* out of range: identity, never garbage */
    CHECK(lut[100] == 100);

    unsigned g = 0;
    CHECK(!gamma_parse("2.2", &g) && g == 220);
    CHECK(!gamma_parse("2.25", &g) && g == 225);
    CHECK(!gamma_parse("1", &g) && g == 100);
    CHECK(!gamma_parse("3.00", &g) && g == 300);
    CHECK(!gamma_parse("off", &g) && g == GAMMA_OFF);
    CHECK(gamma_parse("2.225", &g) && gamma_parse("0.9", &g) && gamma_parse("3.01", &g) && gamma_parse("", &g) &&
          gamma_parse("2.2x", &g) && gamma_parse("-2", &g) && gamma_parse(".5", &g) && gamma_parse("12", &g));
    char b[8];
    gamma_format(b, 220); CHECK(!strcmp(b, "2.20"));
    gamma_format(b, GAMMA_OFF); CHECK(!strcmp(b, "off"));
    printf("gamma: worst %d, %d of %d entries off by one vs libm\n", worst, inexact, 201 * 255);
}

/* Output mapping with dither: black stays black, the cycle average is the 8.8 value to within half a dither step,
 * nothing to dither without gamma/intensity, dither 0 is the plain path, every offset is used once per cycle. */
static void t_dither(void)
{
    static uint16_t l16[256];
    static uint8_t l8[256];
    uint16_t l16_off[256];
    gamma_lut16(l16_off, GAMMA_OFF);
    for (int i = 0; i < 256; i++) CHECK(l16_off[i] == i << 8);
    {   /* 8.8 table rounds to the 8-bit one (within 1 where libm rounding sits on .5) */
        gamma_lut16(l16, 220); gamma_lut(l8, 220);
        int bad = 0;
        for (int i = 0; i < 256; i++) if (abs(((l16[i] + 128) >> 8) - l8[i]) > 1) bad++;
        CHECK(bad == 0 && l16[255] == 65280 && l16[0] == 0);
    }
    int uneven = 0;
    for (unsigned bits = 1; bits <= DITHER_MAX; bits++) {
        unsigned n = 1u << bits;
        /* each pixel sees every offset once per cycle */
        for (uint32_t i = 0; i < 50; i++) {
            uint32_t seen = 0;
            for (uint32_t f = 0; f < n; f++) seen |= 1u << (dither_offset(bits, 1000 + f, i) >> (8 - bits));
            CHECK(seen == (1u << n) - 1);
        }
        int phases = 0;                                   /* neighbours start at different points of the cycle */
        for (uint32_t i = 1; i < 50; i++) phases += dither_offset(bits, 7, i) != dither_offset(bits, 7, i - 1);
        CHECK(phases > 15);
        for (unsigned g = 100; g <= 300; g += 40) {
            gamma_lut16(l16, g);
            for (uint32_t k = 1; k <= 256; k += 51) {
                out_map_t m = { .intensity16 = k << 8, .gamma16 = l16, .dither = (uint8_t)bits };
                for (int b = 0; b < 256; b++) {
                    uint32_t v = l16[b];
                    if (k < 256) v = (v * k) >> 8;
                    pixel_t p = (pixel_t)b * 0x01010101u;
                    uint32_t sum = 0;
                    for (uint32_t f = 0; f < n; f++) {
                        m.frame = f;
                        pixel_t o = out_map_px(&m, p, 3);
                        uneven += (o & 0xFF) != ((o >> 8) & 0xFF) || (o >> 24) != (o & 0xFF);   /* same for every byte */
                        if (b == 0 && o) { printf("  black lit: bits %u g %u k %u\n", bits, g, k); CHECK(0); }
                        sum += o & 0xFF;
                    }
                    int64_t err = (int64_t)sum * 256 - (int64_t)v * n;     /* in 1/256 steps, times n */
                    if (llabs(err) > (int64_t)n * (128 >> bits)) { printf("  avg off: bits %u g %u k %u b %d sum %u v %u\n", bits, g, k, b, sum, v); CHECK(0); }
                }
            }
        }
        out_map_t id = { .intensity16 = OUT_ONE, .gamma16 = l16_off, .dither = (uint8_t)bits };    /* nothing to dither */
        for (uint32_t f = 0; f < n; f++) { id.frame = f; CHECK(out_map_px(&id, 0x80FF7F01u, 9) == 0x80FF7F01u); }
    }
    CHECK(uneven == 0);
    gamma_lut(l8, 250);                                   /* dither 0: gamma table then px_scale, as before */
    out_map_t plain = { .intensity16 = 200 << 8, .gamma = l8 };
    CHECK(out_map_px(&plain, 0x00C08040u, 0) == px_scale(px_lut(0x00C08040u, l8), 200));
    plain.gamma = NULL; plain.intensity16 = OUT_ONE;
    CHECK(out_map_px(&plain, 0x12345678u, 0) == 0x12345678u);
}

/* tri, lerp, rand */
/* (env npix): the length of the channel the program runs on (10 in these tests) */
/* The time-of-day conveniences: derived from hour/minute/second/ms, fractions are decimals 0 <= x < 1. */
static void t_env_time(channel_t *ch)
{
    char e[DSL_ERR_LEN];
    env = (env_t){ .hour = 13, .minute = 45, .second = 30, .ms = 500, .weekday = 3, .timeset = 1, .daylight = 1, .intensity = 256 };
    CHECK(dsl_true(ch, "(eq (env daymin) 825)"));
    CHECK(dsl_true(ch, "(eq (env daysec) 49530)"));               /* above 32767: still an exact int */
    CHECK(dsl_true(ch, "(eq (+ (env daysec) 1) 49531)"));
    CHECK(dsl_true(ch, "(eq (env ms) 500)"));
    CHECK(dsl_true(ch, "(eq (env weekday) 3)"));
    CHECK(dsl_true(ch, "(eq (env timeset) 1)"));
    CHECK(dsl_true(ch, "(eq (env daylight) 1)"));
    CHECK(dsl_true(ch, "(eq (floor (* (env dayfrac) 1000)) 573)"));    /* 49530.5 / 86400 = 0.57327 */
    CHECK(dsl_true(ch, "(eq (floor (* (env hourfrac) 1000)) 758)"));   /* 2730.5 / 3600 = 0.75847 */
    CHECK(dsl_true(ch, "(eq (floor (* (env minfrac) 1000)) 508)"));    /* 30.5 / 60 = 0.50833 */
    env = (env_t){ .hour = 18, .minute = 30, .second = 15, .intensity = 256 };
    CHECK(dsl_true(ch, "(eq (env hourfrac) 0.5)") == false);           /* 30:15 */
    env.second = 0;
    CHECK(dsl_true(ch, "(eq (env hourfrac) 0.5)"));
    env.second = 15;
    CHECK(dsl_true(ch, "(eq (env minfrac) 0.25)"));
    env.minute = 0; env.second = 0;
    CHECK(dsl_true(ch, "(eq (env dayfrac) 0.75)"));
    env = (env_t){ .intensity = 256 };                                    /* midnight */
    CHECK(dsl_true(ch, "(and (eq (env dayfrac) 0) (eq (env hourfrac) 0) (eq (env minfrac) 0) (eq (env daysec) 0))"));
    env = (env_t){ .hour = 23, .minute = 59, .second = 59, .ms = 999, .intensity = 256 };
    CHECK(dsl_true(ch, "(and (lt (env dayfrac) 1) (lt (env hourfrac) 1) (lt (env minfrac) 1) (gt (env dayfrac) 0.9999))"));
    CHECK(dsl_true(ch, "(eq (env daysec) 86399)"));
    env.second = 60;                                                      /* a leap second never reaches 1 */
    CHECK(dsl_true(ch, "(lt (env minfrac) 1)"));
    /* Fractions scale a length directly; whole-number names stay ints for ranges. */
    env = (env_t){ .hour = 12, .intensity = 256 };
    CHECK(run(ch, "(fill (buf O) #000000) (fill (buf O) #FF0000 0 (round (* (env dayfrac) (env npix))))", e) == 0);
    CHECK(ch->buf[BUF_O][4] == 0xFF0000 && ch->buf[BUF_O][5] == 0);
    CHECK(run(ch, "(fill (buf O) #000000) (fill (buf O) #FF0000 0 (/ (env daymin) 144))", e) == 0);
    CHECK(ch->buf[BUF_O][4] == 0xFF0000 && ch->buf[BUF_O][5] == 0);
    env.daylight = -1;
    CHECK(dsl_true(ch, "(lt (env daylight) 0)"));
    CHECK(channel_set_source(ch, "(env dayfrc)", e, sizeof e) != 0 && strstr(e, "did you mean dayfrac?"));
    CHECK(channel_set_source(ch, "(env sunrse)", e, sizeof e) != 0 && strstr(e, "did you mean sunrise?"));
    CHECK(channel_set_source(ch, "(env zzzzzzzz)", e, sizeof e) != 0 && strstr(e, "reference lists them"));
    CHECK(channel_set_source(ch, "(env averyveryverylongname)", e, sizeof e) != 0 && strstr(e, "reference lists them"));
    env = (env_t){ .intensity = 256 };
}

static void t_time_cmp(channel_t *ch)
{
    char e[DSL_ERR_LEN];
    env = (env_t){ .year = 2026, .month = 11, .day = 12, .hour = 18, .minute = 30, .second = 15, .timeset = 1, .intensity = 256 };
    /* minute precision: "18:30" is the whole minute, so gt/eq/lt never overlap */
    CHECK(dsl_true(ch, "(time-eq \"18:30\")"));
    CHECK(!dsl_true(ch, "(time-gt \"18:30\")") && dsl_true(ch, "(time-ge \"18:30\")"));
    CHECK(!dsl_true(ch, "(time-lt \"18:30\")") && dsl_true(ch, "(time-le \"18:30\")"));
    CHECK(dsl_true(ch, "(time-gt \"18:29\")") && dsl_true(ch, "(time-lt \"18:31\")"));
    CHECK(dsl_true(ch, "(time-gt \"8:30\")") && dsl_true(ch, "(time-gt \"08:30\")"));
    /* second precision */
    CHECK(dsl_true(ch, "(time-eq \"18:30:15\")") && !dsl_true(ch, "(time-eq \"18:30:14\")"));
    CHECK(dsl_true(ch, "(time-gt \"18:30:14\")") && dsl_true(ch, "(time-lt \"18:30:16\")"));
    CHECK(dsl_true(ch, "(time-lt \"6:00:00\")") == false);
    /* numbers: minutes after midnight (int = to the minute, decimal = to the second) */
    CHECK(dsl_true(ch, "(time-eq 1110)") && dsl_true(ch, "(time-gt 1109)") && dsl_true(ch, "(time-lt 1111)"));
    CHECK(dsl_true(ch, "(time-eq 1110.25)") && dsl_true(ch, "(time-gt 1110.2)") && !dsl_true(ch, "(time-eq 1110.5)"));
    CHECK(dsl_true(ch, "(time-eq (+ 1440 1110))"));                         /* wraps at 24 h */
    CHECK(!dsl_true(ch, "(time-gt -1)") && !dsl_true(ch, "(time-lt -1)") && !dsl_true(ch, "(time-eq -1)"));
    env.sunset = 1100; env.sunrise = -1;
    CHECK(dsl_true(ch, "(time-gt (env sunset))") && !dsl_true(ch, "(time-gt (env sunrise))"));
    CHECK(dsl_true(ch, "(time-in (env sunset) \"23:00\")") && !dsl_true(ch, "(time-in (env sunrise) \"23:00\")"));
    /* time-in: start inclusive, end exclusive, wraps past midnight, from = to never */
    CHECK(dsl_true(ch, "(time-in \"18:00\" \"19:00\")") && dsl_true(ch, "(time-in \"18:30:15\" \"18:30:16\")"));
    CHECK(!dsl_true(ch, "(time-in \"18:00\" \"18:30:15\")") && !dsl_true(ch, "(time-in \"18:30\" \"18:30\")"));
    CHECK(dsl_true(ch, "(time-in \"18:00\" \"00:00\")") && dsl_true(ch, "(time-in \"18:00\" \"06:00\")"));
    CHECK(!dsl_true(ch, "(time-in \"22:00\" \"06:00\")") && !dsl_true(ch, "(time-in \"06:00\" \"18:00\")"));
    env.hour = 2;
    CHECK(dsl_true(ch, "(time-in \"22:00\" \"06:00\")") && !dsl_true(ch, "(time-in \"22:00\" \"02:30\")") && dsl_true(ch, "(time-in \"22:00\" \"02:30:16\")"));
    env.hour = 0; env.minute = 0; env.second = 0;
    CHECK(dsl_true(ch, "(time-in \"22:00\" \"06:00\")") && dsl_true(ch, "(time-eq \"00:00\")") && !dsl_true(ch, "(time-in \"18:00\" \"00:00\")"));
    env.hour = 23; env.minute = 59; env.second = 59;
    CHECK(dsl_true(ch, "(time-in \"18:00\" \"00:00\")") && dsl_true(ch, "(time-eq \"23:59:59\")") && dsl_true(ch, "(time-ge \"23:59\")"));
    env.hour = 18; env.minute = 30; env.second = 15;
    /* dates: "MM-DD" every year, "YYYY-MM-DD" one day */
    CHECK(dsl_true(ch, "(date-eq \"11-12\")") && dsl_true(ch, "(date-eq \"2026-11-12\")") && !dsl_true(ch, "(date-eq \"2025-11-12\")"));
    CHECK(dsl_true(ch, "(date-gt \"11-11\")") && dsl_true(ch, "(date-lt \"12-01\")") && dsl_true(ch, "(date-ge \"11-12\")"));
    CHECK(dsl_true(ch, "(date-le \"11-12\")") && !dsl_true(ch, "(date-gt \"11-12\")") && !dsl_true(ch, "(date-lt \"11-12\")"));
    CHECK(dsl_true(ch, "(date-gt \"2025-12-31\")") && dsl_true(ch, "(date-lt \"2027-01-01\")") && !dsl_true(ch, "(date-gt \"2026-12-01\")"));
    CHECK(dsl_true(ch, "(date-lt \"12-01\")") && !dsl_true(ch, "(date-lt \"01-06\")"));          /* "MM-DD" never wraps */
    /* date-in: both ends inclusive; "MM-DD" pairs wrap past the year end */
    CHECK(dsl_true(ch, "(date-in \"11-01\" \"11-30\")") && dsl_true(ch, "(date-in \"11-12\" \"11-12\")"));
    CHECK(dsl_true(ch, "(date-in \"11-12\" \"11-13\")") && dsl_true(ch, "(date-in \"11-01\" \"11-12\")"));
    CHECK(!dsl_true(ch, "(date-in \"12-01\" \"01-06\")") && dsl_true(ch, "(date-in \"11-01\" \"02-28\")"));
    CHECK(dsl_true(ch, "(date-in \"2026-01-01\" \"2026-12-31\")") && !dsl_true(ch, "(date-in \"2026-12-01\" \"2026-01-06\")"));
    env.month = 1; env.day = 3;
    CHECK(dsl_true(ch, "(date-in \"12-01\" \"01-06\")") && !dsl_true(ch, "(date-in \"12-01\" \"01-02\")"));
    env.month = 12; env.day = 31;
    CHECK(dsl_true(ch, "(date-in \"12-01\" \"01-06\")") && dsl_true(ch, "(date-in \"12-31\" \"12-31\")"));
    env.month = 2; env.day = 29; env.year = 2028;
    CHECK(dsl_true(ch, "(date-eq \"02-29\")") && dsl_true(ch, "(date-eq \"2028-02-29\")"));
    /* strings made at run time work too; a bad one is a counted fault */
    env.month = 11; env.day = 12; env.year = 2026;
    CHECK(dsl_true(ch, "(time-eq (str-cat \"18:\" \"30\"))") && dsl_true(ch, "(date-eq (str-cat \"11-\" (str-num 12)))"));
    ch->perf.faults = 0;
    CHECK(run(ch, "(if (time-eq (str-cat \"18\" \"30\")) (fill (buf O) #FFFFFF))", e) == 0);
    CHECK(ch->perf.faults == 1 && ch->perf.last_err && strstr(ch->perf.last_err, "HH:MM"));
    CHECK(run(ch, "(if (date-in (str-cat \"11-\" \"01\") \"2026-12-01\") (fill (buf O) #FFFFFF))", e) == 0);
    CHECK(ch->perf.faults == 2 && strstr(ch->perf.last_err, "both dates"));
    ch->perf.faults = 0;
    /* nil while the clock isn't set, whatever it reads */
    env.timeset = 0;
    CHECK(!dsl_true(ch, "(time-eq \"18:30\")") && !dsl_true(ch, "(time-in \"00:00\" \"23:59:59\")") && !dsl_true(ch, "(date-eq \"11-12\")"));
    CHECK(!dsl_true(ch, "(date-in \"01-01\" \"12-31\")") && !dsl_true(ch, "(time-ge 0)") && dsl_true(ch, "(not (time-lt \"18:31\"))"));
    CHECK(ch->perf.faults == 0);
    /* compile errors point at the bad literal */
    struct { const char *src, *want; } bad[] = {
        { "(time-gt \"18.30\")", "time-gt: a time is written" },     { "(time-gt \"24:00\")", "no such time" },
        { "(time-gt \"18:60\")", "no such time" },                    { "(time-eq \"18:30:61\")", "no such time" },
        { "(time-gt \"183:0\")", "a time is written" },               { "(time-gt \"\")", "a time is written" },
        { "(time-in \"18:00\" \"6\")", "time-in: a time is written" }, { "(time-gt #FF0000)", "minutes after midnight" },
        { "(time-gt t)", "minutes after midnight" },                  { "(date-eq \"11/12\")", "date-eq: a date is written" },
        { "(date-eq \"13-01\")", "no such date" },                    { "(date-eq \"04-31\")", "no such date" },
        { "(date-eq \"2026-02-29\")", "not a leap year" },            { "(date-eq \"1969-12-31\")", "start at 1970" },
        { "(date-in \"11-01\" \"2026-12-01\")", "both dates" },       { "(date-eq 1112)", NULL },
        { "(time-gt)", NULL },                                        { "(time-in \"18:00\")", NULL },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        bool failed = channel_set_source(ch, bad[i].src, e, sizeof e) != 0;
        CHECK(failed && (!bad[i].want || strstr(e, bad[i].want)));
        if (!failed || (bad[i].want && !strstr(e, bad[i].want))) printf("  %s -> %s\n", bad[i].src, failed ? e : "(compiled)");
    }
    CHECK(channel_set_source(ch, "(def s \"18:30\") (if (time-eq s) (fill (buf O) #FFFFFF))", e, sizeof e) == 0);
    CHECK(channel_set_source(ch, "(def x 0) (if (time-in \"22:00\" \"06:00\") (fill (buf O) #FFFFFF))", e, sizeof e) == 0);
    env = (env_t){ .intensity = 256 };
}

static void t_env_npix(channel_t *ch)
{
    char e[DSL_ERR_LEN];
    CHECK(dsl_true(ch, "(eq (env npix) 10)"));
    CHECK(run(ch, "(fill (buf O) #000000) (set (buf O) (- (env npix) 1) #FFFFFF)", e) == 0 && ch->buf[BUF_O][9] == 0xFFFFFF);
    CHECK(run(ch, "(fill (buf O) #000000) (fill (buf O) #FF0000 (/ (env npix) 2))", e) == 0 && ch->buf[BUF_O][4] == 0 && ch->buf[BUF_O][5] == 0xFF0000);
    CHECK(channel_set_source(ch, "(env npixx)", e, sizeof e) != 0 && strstr(e, "npix"));
}

/* seg7: masks of numbers on a 7-segment display wired as one strip. The test channel has 10 pixels; G buffers 10 too. */
static uint32_t lit_mask(const pixel_t *p, int n) { uint32_t m = 0; for (int i = 0; i < n; i++) if (p[i] == 0xFFFFFFFFu) m |= 1u << i; else if (p[i]) return 0xDEADu; return m; }

static void t_seg7(channel_t *ch)
{
    char e[DSL_ERR_LEN];
    /* one digit, abcdefg, 1 px per segment: bit i = segment a+i */
    CHECK(run(ch, "(fill (buf O) #123456) (seg7 (slice (buf O) 0 7) 0)", e) == 0 && lit_mask(ch->buf[BUF_O], 7) == 0x3F);
    CHECK(ch->buf[BUF_O][7] == 0x123456);                                           /* after the digit: untouched */
    CHECK(run(ch, "(seg7 (slice (buf O) 0 7) 8)", e) == 0 && lit_mask(ch->buf[BUF_O], 7) == 0x7F);
    CHECK(run(ch, "(seg7 (slice (buf O) 0 7) 7)", e) == 0 && lit_mask(ch->buf[BUF_O], 7) == 0x07);
    /* another wiring order: the strip goes f a b c d e g */
    CHECK(run(ch, "(seg7 (slice (buf O) 0 7) 7 fabcdeg)", e) == 0 && lit_mask(ch->buf[BUF_O], 7) == 0x0E);   /* a b c at 1 2 3 */
    /* strides: a d g 2 px, b c e f 1 px -> 10 px: a a b c d d e f g g */
    CHECK(run(ch, "(seg7 (buf O) 4 abcdefg 2 1)", e) == 0 && lit_mask(ch->buf[BUF_O], 10) == ((1u << 2) | (1u << 3) | (1u << 7) | (1u << 8) | (1u << 9)));
    /* two digits of 5 px (h 1, v... no: use G buffers as a scratch strip of 10 with the 4-px "abcdefg" impossible) */
    for (int i = 0; i < 10; i++) globals.buf[4][i] = 0;
    CHECK(run(ch, "(seg7 (slice (buf G4) 0 7) 3 _ _ _ 1)", e) == 0 && lit_mask(globals.buf[4], 7) == 0x4F);
    /* right-aligned with blanks, minus, overflow dashes: one 7-px digit fits the first 7 of 10 px */
    CHECK(run(ch, "(seg7 (buf O) -5)", e) == 0 && lit_mask(ch->buf[BUF_O], 7) == 0x40);           /* "-5" doesn't fit 1 digit */
    CHECK(run(ch, "(seg7 (buf O) 12)", e) == 0 && lit_mask(ch->buf[BUF_O], 7) == 0x40);           /* too long: dash */
    CHECK(run(ch, "(seg7 (buf O) 42 abcdefgp)", e) == 0);                                        /* 8 px per digit: 1 digit */
    CHECK(lit_mask(ch->buf[BUF_O], 8) == 0x40);
    /* multi-digit on a longer strip: G buffers in a 70-px channel */
    channel_t big;
    CHECK(channel_init(&big, 21, 512, &env, &globals) == 0);
    CHECK(channel_set_source(&big, "(fill (buf O) #000000) (seg7 (buf O) 7)", e, sizeof e) == 0);
    channel_render(&big);
    CHECK(lit_mask(big.buf[BUF_O], 7) == 0 && lit_mask(big.buf[BUF_O] + 7, 7) == 0 && lit_mask(big.buf[BUF_O] + 14, 7) == 0x07);   /* "  7" */
    CHECK(channel_set_source(&big, "(seg7 (buf O) -42)", e, sizeof e) == 0);
    channel_render(&big);
    CHECK(lit_mask(big.buf[BUF_O], 7) == 0x40 && lit_mask(big.buf[BUF_O] + 7, 7) == 0x66 && lit_mask(big.buf[BUF_O] + 14, 7) == 0x5B);
    CHECK(channel_set_source(&big, "(seg7 (buf O) 1234)", e, sizeof e) == 0);
    channel_render(&big);
    CHECK(lit_mask(big.buf[BUF_O], 21) == 0x40u * (1 | 1u << 7 | 1u << 14));                     /* "---" */
    CHECK(channel_set_source(&big, "(seg7 (buf O) 5 _ _ _ 2)", e, sizeof e) == 0);                /* 2 digits, 3rd untouched */
    big.buf[BUF_O][20] = 0x00ABCDEF;
    channel_render(&big);
    CHECK(lit_mask(big.buf[BUF_O] + 7, 7) == 0x6D && big.buf[BUF_O][20] == 0x00ABCDEF);
    channel_free(&big);
    /* compile-time checks */
    CHECK(channel_set_source(ch, "(seg7 (buf O) 1 abcdef)", e, sizeof e) != 0 && strstr(e, "layout"));      /* g missing */
    CHECK(channel_set_source(ch, "(seg7 (buf O) 1 abcdefgg)", e, sizeof e) != 0);
    CHECK(channel_set_source(ch, "(seg7 (buf O) 1 abcdefgx)", e, sizeof e) != 0);
    CHECK(channel_set_source(ch, "(seg7 (buf O) 1 _ 0)", e, sizeof e) != 0 && strstr(e, "stride"));
    CHECK(channel_set_source(ch, "(seg7 (buf O) 1.5)", e, sizeof e) != 0);                                 /* ints only */
    CHECK(channel_set_source(ch, "(seg7 (buf O) (floor (* 1.5 2)))", e, sizeof e) == 0);
    uint32_t f0 = ch->perf.faults;
    CHECK(run(ch, "(seg7 (buf O) 1 _ (env second))", e) == 0 && ch->perf.faults == f0 + 1);               /* run-time stride 0: fault */
    /* the font: every digit, and the displayable letters */
    CHECK(dsl_seg_glyph(7, '0') == 0x3F && dsl_seg_glyph(7, '9') == 0x6F && dsl_seg_glyph(7, 'A') == 0x77 && dsl_seg_glyph(7, 'b') == 0x7C);
    CHECK(dsl_seg_glyph(7, 'B') == 0x7C && dsl_seg_glyph(7, 'a') == 0x5F && dsl_seg_glyph(7, 'K') == 0 && dsl_seg_glyph(7, 'm') == 0);
    CHECK(dsl_seg_glyph(7, ' ') == 0 && dsl_seg_glyph(7, '-') == 0x40 && dsl_seg_glyph(7, '.') == 0x80 && dsl_seg_glyph(7, 's') == 0x6D);
}

/* Strings: literals, views, formatting. Compared with eq (string equality). */
static void t_strings(channel_t *ch)
{
    char e[DSL_ERR_LEN];
    env_t saved = env;
    env.hour = 14; env.minute = 5; env.second = 9; env.year = 2026; env.month = 3; env.day = 7; env.doy = 66;
    struct { const char *expr, *want; } eqs[] = {
        { "\"ab\"", "\"ab\"" }, { "(str-sub \"hello\" 1 3)", "\"el\"" }, { "(str-sub \"hello\" -3)", "\"llo\"" },
        { "(str-sub \"abc\" 2 1)", "\"\"" }, { "(str-sub \"abc\" _ -1)", "\"ab\"" }, { "(str-sub \"abc\" -10 10)", "\"abc\"" },
        { "(str-len \"hello\")", "5" }, { "(str-len \"\")", "0" }, { "(str-len \"a\\\"b\\\\c\")", "5" },
        { "(str-num 42)", "\"42\"" }, { "(str-num -7 4)", "\"  -7\"" }, { "(str-num -7 4 _ t)", "\"-007\"" },
        { "(str-num 1.5)", "\"1.50\"" }, { "(str-num 2.5 _ 0)", "\"3\"" }, { "(str-num -2.5 _ 0)", "\"-3\"" },
        { "(str-num -0.001 _ 2)", "\"0.00\"" }, { "(str-num 3 _ 2)", "\"3.00\"" }, { "(str-num 0.25 6 1 t)", "\"0000.3\"" },
        { "(str-num 7 1)", "\"7\"" }, { "(str-num 123 2)", "\"123\"" }, { "(str-num -1.25 _ 1)", "\"-1.3\"" },
        { "(str-num 2147483647)", "\"2147483647\"" }, { "(str-num (- -2147483647 9))", "\"-2147483647\"" },
        { "(str-num 32767.5 _ 4)", "\"32767.5000\"" }, { "(str-num 0.00001 _ 4)", "\"0.0000\"" },
        { "(str-time \"%H:%M:%S\")", "\"14:05:09\"" }, { "(str-time \"%I %l %p\")", "\"02  2 PM\"" },
        { "(str-time \"%Y-%m-%d %j\")", "\"2026-03-07 066\"" }, { "(str-time \"%y %e %%\")", "\"26  7 %\"" },
        { "(str-time \"%k:%M\")", "\"14:05\"" }, { "(str-time \"\")", "\"\"" }, { "(str-time \"no codes\")", "\"no codes\"" },
        { "(str-cat \"T\" 5)", "\"T5\"" }, { "(str-cat 1.5 \"x\" -2)", "\"1.50x-2\"" }, { "(str-cat \"\" \"\")", "\"\"" },
        { "(if t \"a\" \"b\")", "\"a\"" }, { "(str-sub (str-cat \"ab\" \"cd\") 1 3)", "\"bc\"" },
    };
    for (size_t i = 0; i < sizeof eqs / sizeof eqs[0]; i++) {
        char cond[200];
        snprintf(cond, sizeof cond, "(eq %s %s)", eqs[i].expr, eqs[i].want);
        if (!dsl_true(ch, cond)) { printf("  %s != %s\n", eqs[i].expr, eqs[i].want); CHECK(0); }
    }
    CHECK(dsl_true(ch, "(ne \"ab\" \"abc\")") && dsl_true(ch, "(ne \"ab\" \"aB\")") && !dsl_true(ch, "(ne \"\" \"\")"));
    env.hour = 9;  CHECK(dsl_true(ch, "(eq (str-time \"%k|%H|%l\") \" 9|09| 9\")"));
    env.hour = 0;  CHECK(dsl_true(ch, "(eq (str-time \"%I %p %H\") \"12 AM 00\")"));
    env.hour = 12; CHECK(dsl_true(ch, "(eq (str-time \"%I %p\") \"12 PM\")"));
    /* variables hold strings for the run; str-cat stops at DSL_STR_MAX */
    CHECK(run(ch, "(def s (str-cat \"T\" (env hour))) (if (eq s \"T12\") (fill (buf O) #00FF00) (fill (buf O) #FF0000))", e) == 0 && ch->buf[BUF_O][0] == 0x00FF00);
    char src[400];
    snprintf(src, sizeof src, "(def a \"%0100d\") (if (eq (str-len (str-cat a a a)) %d) (fill (buf O) #00FF00) (fill (buf O) #FF0000))", 0, DSL_STR_MAX);
    CHECK(run(ch, src, e) == 0 && ch->buf[BUF_O][0] == 0x00FF00);
    /* compile errors */
    CHECK(channel_set_source(ch, "(str-time \"%Q\")", e, sizeof e) != 0 && strstr(e, "unknown"));
    CHECK(channel_set_source(ch, "(str-time \"abc%\")", e, sizeof e) != 0 && strstr(e, "end"));
    CHECK(channel_set_source(ch, "(str-num 1 99)", e, sizeof e) != 0 && strstr(e, "width"));
    CHECK(channel_set_source(ch, "(str-num 1 _ 9)", e, sizeof e) != 0 && strstr(e, "decimals"));
    CHECK(channel_set_source(ch, "(str-cat \"a\" t)", e, sizeof e) != 0 && strstr(e, "col 14"));
    CHECK(channel_set_source(ch, "(str-len 5)", e, sizeof e) != 0 && strstr(e, "expected string"));
    CHECK(channel_set_source(ch, "(fill (buf O) \"red\")", e, sizeof e) != 0 && strstr(e, "got string"));
    CHECK(channel_set_source(ch, "(eq \"a\" 1)", e, sizeof e) != 0 && strstr(e, "strings"));
    CHECK(channel_set_source(ch, "(str-len \"abc)", e, sizeof e) != 0 && strstr(e, "unclosed"));
    CHECK(channel_set_source(ch, "(str-len \"ab\ncd\")", e, sizeof e) != 0 && strstr(e, "unclosed"));
    CHECK(channel_set_source(ch, "(str-len \"a\\nb\")", e, sizeof e) != 0 && strstr(e, "escape") && strstr(e, "col 12"));
    CHECK(channel_set_source(ch, "(str-len \"a\"b)", e, sizeof e) != 0 && strstr(e, "after the string"));
    CHECK(channel_set_source(ch, "\"top\"", e, sizeof e) != 0 && strstr(e, "top-level"));
    CHECK(channel_set_source(ch, "(buf \"X\")", e, sizeof e) != 0);
    snprintf(src, sizeof src, "(str-len \"%0*d\")", DSL_STR_MAX, 0);
    CHECK(channel_set_source(ch, src, e, sizeof e) == 0);
    snprintf(src, sizeof src, "(str-len \"%0*d\")", DSL_STR_MAX + 1, 0);
    CHECK(channel_set_source(ch, src, e, sizeof e) != 0 && strstr(e, "too long"));
    CHECK(channel_set_source(ch, "(str-len \"a;b\") ; \"comment\"", e, sizeof e) == 0);      /* ; inside a string isn't a comment */
    /* run-time faults: a format made at run time, and running out of scratch */
    uint32_t f0 = ch->perf.faults;
    CHECK(run(ch, "(str-time (if (gt (env second) 100) \"%H\" \"%Q\"))", e) == 0 && ch->perf.faults == f0 + 1);
    channel_t tiny;
    if (channel_init(&tiny, 10, 16, &env, &globals) == 0) {
        CHECK(channel_set_source(&tiny, "(str-num 1 32)", e, sizeof e) == 0);
        channel_render(&tiny);
        CHECK(tiny.perf.faults == 1 && tiny.perf.last_err && strstr(tiny.perf.last_err, "scratch"));
        channel_free(&tiny);
    }
    env = saved;
}

/* seg7 with text, seg9 and seg14 (same engine as seg7) on a 30-px channel. */
static void t_seg_text(channel_t *ch)
{
    (void)ch;
    char e[DSL_ERR_LEN];
    channel_t b;
    CHECK(channel_init(&b, 30, 512, &env, &globals) == 0);
    pixel_t *o = b.buf[BUF_O];
#define SEG(src) (channel_set_source(&b, src, e, sizeof e) == 0 ? (channel_render(&b), 1) : (printf("  %s: %s\n", src, e), 0))
    /* seg7 text: left-aligned, cut off, points join the character before */
    CHECK(SEG("(seg7 (slice (buf O) 0 7) \"A\")") && lit_mask(o, 7) == 0x77);
    CHECK(SEG("(seg7 (slice (buf O) 0 21) \"Ab\")") && lit_mask(o, 7) == 0x77 && lit_mask(o + 7, 7) == 0x7C && lit_mask(o + 14, 7) == 0);
    CHECK(SEG("(seg7 (slice (buf O) 0 21) \"ABCD\")") && lit_mask(o + 14, 7) == 0x39);
    CHECK(SEG("(seg7 (slice (buf O) 0 16) \"1.2\" abcdefgp)") && lit_mask(o, 8) == 0x86 && lit_mask(o + 8, 8) == 0x5B);
    CHECK(SEG("(seg7 (slice (buf O) 0 16) \".5\" abcdefgp)") && lit_mask(o, 8) == 0x80 && lit_mask(o + 8, 8) == 0x6D);
    CHECK(SEG("(seg7 (slice (buf O) 0 16) \"..\" abcdefgp)") && lit_mask(o, 8) == 0x80 && lit_mask(o + 8, 8) == 0x80);
    CHECK(SEG("(seg7 (slice (buf O) 0 16) \"12.3\" abcdefgp)") && lit_mask(o, 8) == 0x06 && lit_mask(o + 8, 8) == 0xDB);   /* 3 cut */
    CHECK(SEG("(seg7 (slice (buf O) 0 16) \"1.2.\" abcdefgp)") && lit_mask(o + 8, 8) == 0xDB);   /* the last point still joins */
    CHECK(SEG("(seg7 (slice (buf O) 0 14) (str-time \"%H\"))"));
    /* seg9: centre verticals j m (default layout abcdefgjm, 9 px) */
    CHECK(SEG("(seg9 (slice (buf O) 0 9) \"T\")") && lit_mask(o, 9) == 0x181);
    CHECK(SEG("(seg9 (slice (buf O) 0 9) \"M\")") && lit_mask(o, 9) == 0x0B7);
    CHECK(SEG("(seg9 (slice (buf O) 0 9) \"8\")") && lit_mask(o, 9) == 0x07F);
    CHECK(SEG("(seg9 (slice (buf O) 0 9) \"k\")") && lit_mask(o, 9) == 0x0F4);
    CHECK(SEG("(seg9 (slice (buf O) 0 10) \"1.\" abcdefgjmp)") && lit_mask(o, 10) == 0x206);
    CHECK(SEG("(seg9 (slice (buf O) 0 9) \"T\" jmabcdefg)") && lit_mask(o, 9) == 0x007);        /* j m a first */
    CHECK(SEG("(seg9 (buf O) 42)") && lit_mask(o, 9) == 0 && lit_mask(o + 9, 9) == 0x66 && lit_mask(o + 18, 9) == 0x5B);
    /* seg14: default layout a b c d e f g1 g2 h j k l m n, 14 px */
    CHECK(SEG("(seg14 (slice (buf O) 0 14) \"A\")") && lit_mask(o, 14) == 0x00F7);
    CHECK(SEG("(seg14 (slice (buf O) 0 14) \"x\")") && lit_mask(o, 14) == 0x2D00);
    CHECK(SEG("(seg14 (slice (buf O) 0 14) 1)") && lit_mask(o, 14) == 0x0006);
    CHECK(SEG("(seg14 (slice (buf O) 0 14) \"A\" nmlkjhg2g1fedcba)") && lit_mask(o, 14) == 0x3BC0);   /* reversed */
    CHECK(SEG("(seg14 (slice (buf O) 0 14) \"A\" ABCDEFG1G2HJKLMN)") && lit_mask(o, 14) == 0x00F7);
    CHECK(SEG("(seg14 (slice (buf O) 0 15) \"1.\" abcdefg1g2hjklmnp)") && lit_mask(o, 15) == 0x4006);
    /* strides: a d hstride, b c e f j m vstride, h k l n dstride, g1 g2 gstride */
    for (int i = 0; i < 30; i++) o[i] = 0x00123456;
    CHECK(SEG("(seg14 (buf O) \"-\" _ 2 1 3 1)") && lit_mask(o, 24) == 0x300 && o[24] == 0x00123456);   /* 24 px: one digit */
    for (int i = 0; i < 30; i++) o[i] = 0x00123456;
    CHECK(SEG("(seg14 (buf O) \"-\" _ 3)") && lit_mask(o, 20) == 0x3C00 && o[20] == 0x00123456);   /* gstride (3+1)/2 = 2 */
    CHECK(SEG("(seg14 (buf O) \"ab\" _ _ _ _ _ 2)") && lit_mask(o, 14) == 0x00F7 && lit_mask(o + 14, 14) == 0x128F);
    channel_free(&b);
#undef SEG
    /* compile-time checks */
    CHECK(channel_set_source(ch, "(seg14 (buf O) 1 abcdefghjklmn)", e, sizeof e) != 0 && strstr(e, "g1 g2"));
    CHECK(channel_set_source(ch, "(seg14 (buf O) 1 abcdefg1g1hjklmn)", e, sizeof e) != 0);
    CHECK(channel_set_source(ch, "(seg9 (buf O) 1 abcdefg)", e, sizeof e) != 0 && strstr(e, "j m"));
    CHECK(channel_set_source(ch, "(seg14 (buf O) 1.5)", e, sizeof e) != 0 && strstr(e, "str-num"));
    CHECK(channel_set_source(ch, "(seg7 (buf O) #FF0000)", e, sizeof e) != 0 && strstr(e, "int or a string"));
    CHECK(channel_set_source(ch, "(seg14 (buf O) 1 _ _ _ 0)", e, sizeof e) != 0 && strstr(e, "stride"));
    CHECK(channel_set_source(ch, "(seg14 (buf O) 1 _ _ _ _ 0)", e, sizeof e) != 0 && strstr(e, "stride"));
    CHECK(channel_set_source(ch, "(seg14 (buf O) 1 _ _ _ _ _ 0)", e, sizeof e) != 0 && strstr(e, "digits"));
    /* the fonts */
    CHECK(dsl_seg_glyph(14, 'a') == dsl_seg_glyph(14, 'A') && dsl_seg_glyph(14, '~') == 0 && dsl_seg_glyph(14, '.') == 0x4000);
    CHECK(dsl_seg_glyph(9, 't') == 0x78 && dsl_seg_glyph(9, 'T') == 0x181 && dsl_seg_glyph(9, '.') == 0x200 && dsl_seg_glyph(9, 'K') == 0xF4);
    CHECK(dsl_seg_glyph(9, 'A') == 0x77 && dsl_seg_glyph(7, 'M') == 0 && dsl_seg_glyph(7, 'm') == 0 && dsl_seg_glyph(14, (char)200) == 0);
    for (int c = '0'; c <= '9'; c++) CHECK(dsl_seg_glyph(14, (char)c) != 0 && dsl_seg_glyph(9, (char)c) == dsl_seg_glyph(7, (char)c));
    for (int c = 'A'; c <= 'Z'; c++) CHECK(dsl_seg_glyph(14, (char)c) != 0 && dsl_seg_glyph(9, (char)c) != 0);
}

static void t_math_helpers(channel_t *ch)
{
    char e[DSL_ERR_LEN];
    struct { const char *expr, *want; } eqs[] = {
        { "(tri 0)", "0" }, { "(tri 90)", "1" }, { "(tri 45)", "0.5" }, { "(tri 135)", "0.5" }, { "(tri 180)", "0" },
        { "(tri 225)", "-0.5" }, { "(tri 270)", "-1" }, { "(tri 315)", "-0.5" }, { "(tri 360)", "0" }, { "(tri -90)", "-1" },
        { "(tri 22.5)", "0.25" }, { "(tri 3600090)", "1" }, { "(tri -450)", "-1" },
        { "(lerp 0 10 0.5)", "5" }, { "(lerp 0 10 2)", "20" }, { "(lerp 10 0 0.25)", "7.5" }, { "(lerp 0 10 -0.5)", "-5" },
        { "(lerp 1.5 2.5 0.5)", "2" }, { "(lerp 0 10 1)", "10" }, { "(lerp -4 4 3)", "20" }, { "(lerp 7 7 123.5)", "7" },
        { "(lerp #FF0000 #0000FF 0.5)", "#800080" }, { "(lerp #FF0000 #0000FF 2)", "#0000FF" },
        { "(lerp #FF0000 #0000FF -1)", "#FF0000" }, { "(lerp #000000 #FFFFFF 0.25)", "#404040" },
        { "(lerp #102030 #102030 0.7)", "#102030" },
        { "(lerp 0 10 100000)", "1000000" },                                       /* ints: full range */
    };
    for (size_t i = 0; i < sizeof eqs / sizeof eqs[0]; i++) {
        char cond[160];
        snprintf(cond, sizeof cond, "(eq %s %s)", eqs[i].expr, eqs[i].want);
        if (!dsl_true(ch, cond)) { printf("  %s != %s\n", eqs[i].expr, eqs[i].want); CHECK(0); }
    }
    CHECK(dsl_true(ch, "(gt (lerp 0 100000 0.5) 32767)"));                         /* decimal: saturates, no wrap */
    CHECK(dsl_true(ch, "(lt (lerp 0 -100000 0.5) -32767)"));
    CHECK(dsl_true(ch, "(gt (lerp 0 2147483647 30000.5) 32767)"));
    CHECK(dsl_true(ch, "(and (gt (tri 10) 0.111) (lt (tri 10) 0.112))"));     /* 10/90 */
    /* types: int lerp works as an index, decimal lerp/tri/rand don't; mixed kinds are rejected */
    CHECK(run(ch, "(set (buf O) (lerp 0 4 1) #FFFFFF)", e) == 0);
    CHECK(channel_set_source(ch, "(set (buf O) (lerp 0 4 0.5) #FFFFFF)", e, sizeof e) != 0 && strstr(e, "expected"));
    CHECK(channel_set_source(ch, "(lerp #FF0000 5 0.5)", e, sizeof e) != 0 && strstr(e, "two numbers or two colors"));
    CHECK(channel_set_source(ch, "(lerp #FF0000 #00FF00 #0000FF)", e, sizeof e) != 0);
    CHECK(channel_set_source(ch, "(fill (buf O) (lerp 1 2 0.5))", e, sizeof e) != 0);
    CHECK(run(ch, "(fill (buf O) (lerp #FF0000 #0000FF (/ (env frame) 100.0)))", e) == 0);
    CHECK(channel_set_source(ch, "(set (buf O) (rand) #FFFFFF)", e, sizeof e) != 0);
    CHECK(channel_set_source(ch, "(rand _ 5)", e, sizeof e) != 0 && strstr(e, "no default"));
    CHECK(channel_set_source(ch, "(rand 1 2 3)", e, sizeof e) != 0);

    /* rand: bounds, every value hit about evenly, swapped and equal limits, decimals */
    int hist[10] = { 0 }, outside = 0;
    for (int i = 0; i < 2000; i++) {
        CHECK(run(ch, "(fill (buf O) #000000) (set (buf O) (rand 10) #FFFFFF)", e) == 0);
        int lit = -1, n = 0;
        for (int p = 0; p < 10; p++) if (ch->buf[BUF_O][p]) { lit = p; n++; }
        if (n == 1) hist[lit]++; else outside++;
    }
    CHECK(outside == 0);
    for (int p = 0; p < 10; p++) if (hist[p] < 130 || hist[p] > 270) { printf("  rand 10: %d hit %d times\n", p, hist[p]); CHECK(0); }
    unsigned seen = 0;
    for (int i = 0; i < 400; i++) {
        CHECK(run(ch, i & 1 ? "(fill (buf O) #000000) (set (buf O) (rand 3 7) #FFFFFF)"
                            : "(fill (buf O) #000000) (set (buf O) (rand 7 3) #FFFFFF)", e) == 0);
        for (int p = 0; p < 10; p++) if (ch->buf[BUF_O][p]) seen |= 1u << p;
    }
    CHECK(seen == 0x078);                                                            /* 3..6 only, all of them */
    CHECK(dsl_true(ch, "(eq (rand 5 5) 5)") && dsl_true(ch, "(eq (rand 0) 0)"));
    int ok = 0;
    for (int i = 0; i < 300; i++)
        ok += dsl_true(ch, "(and (ge (rand) 0) (lt (rand) 1) (ge (rand 0.5 1.5) 0.5) (lt (rand 0.5 1.5) 1.5) (ge (rand -2) -2) (lt (rand -2) 0))");
    CHECK(ok == 300);
    int differ = 0;                                                                  /* not folded: every call is new */
    for (int i = 0; i < 20; i++) differ += dsl_true(ch, "(ne (rand 1000000) (rand 1000000))");
    CHECK(differ >= 19);
    int lowhalf = 0;
    for (int i = 0; i < 400; i++) lowhalf += dsl_true(ch, "(lt (rand) 0.5)");
    CHECK(lowhalf > 150 && lowhalf < 250);
}

/* Wire formats: parse, normalize, encode. */
static void t_wire(void)
{
    wire_t w; char norm[WIRE_STR_MAX]; uint8_t out[WIRE_MAX];
    CHECK(!wire_parse("rgb", &w, norm) && w.n == 3 && !strcmp(norm, "RGB"));
    wire_encode(&w, 0x44112233u, out);
    CHECK(out[0] == 0x11 && out[1] == 0x22 && out[2] == 0x33);
    CHECK(!wire_parse("GRB", &w, norm));
    wire_encode(&w, 0x44112233u, out);
    CHECK(out[0] == 0x22 && out[1] == 0x11 && out[2] == 0x33);
    CHECK(!wire_parse("  BRG   www ", &w, norm) && w.n == 6 && !strcmp(norm, "BRG WWW"));    /* Govee S14 */
    wire_encode(&w, 0x80112233u, out);
    CHECK(out[0] == 0x33 && out[1] == 0x11 && out[2] == 0x22 && out[3] == 0x80 && out[4] == 0x80 && out[5] == 0x80);
    CHECK(!wire_parse("GRBW", &w, norm) && w.n == 4);                                          /* SK6812 RGBW */
    wire_encode(&w, 0x80112233u, out);
    CHECK(out[0] == 0x22 && out[1] == 0x11 && out[2] == 0x33 && out[3] == 0x80);
    CHECK(!wire_parse("RGB,W00", &w, norm) && !strcmp(norm, "RGB W00"));
    wire_encode(&w, 0x80112233u, out);
    CHECK(out[3] == 0x80 && out[4] == 0 && out[5] == 0);
    CHECK(!wire_parse("RGBW RGBW RGBW", &w, norm) && w.n == 12 && !strcmp(norm, "RGBW RGBW RGBW"));
    CHECK(wire_parse("RGBW RGBW RGBW R", &w, norm) != NULL);                                   /* > 12 bytes */
    CHECK(wire_parse("RGX", &w, norm) != NULL && wire_parse("", &w, norm) != NULL && wire_parse("  ", &w, norm) != NULL);
}

/* Several W outputs: W spread over them (finer steps), white limit, dither, black stays black. */
static void t_wire_white(void)
{
    static uint16_t l16[256], id16[256];
    wire_t s14, one, rgbw; char norm[WIRE_STR_MAX]; uint8_t out[WIRE_MAX], ref[WIRE_MAX];
    CHECK(!wire_parse("BRG WWW", &s14, norm) && s14.nw == 3);
    CHECK(!wire_parse("GRBW", &one, norm) && one.nw == 1);
    CHECK(!wire_parse("RGB", &rgbw, norm) && rgbw.nw == 0);
    gamma_lut16(id16, GAMMA_OFF);
    out_map_t m = { .intensity16 = OUT_ONE, .gamma16 = id16, .wmax = 256 };

    /* no gamma/intensity/limit: W 255 = all three at 255, 0 = off, RGB untouched */
    wire_encode_mapped(&s14, &m, 0xFF112233u, 0, out);
    CHECK(out[0] == 0x33 && out[1] == 0x11 && out[2] == 0x22 && out[3] == 255 && out[4] == 255 && out[5] == 255);
    wire_encode_mapped(&s14, &m, 0x00112233u, 0, out);
    CHECK(out[3] == 0 && out[4] == 0 && out[5] == 0);
    wire_encode_mapped(&s14, &m, 0x01000000u, 0, out);                  /* W 1 = 3 steps of the 765 */
    CHECK(out[3] == 1 && out[4] == 1 && out[5] == 1);

    /* with gamma: totals use all 765 steps, spread evenly (first ones get the remainder), monotonic */
    gamma_lut16(l16, 220);
    m.gamma16 = l16;
    uint32_t last = 0; int uneven = 0, fine = 0;
    for (uint32_t wv = 0; wv < 256; wv++) {
        wire_encode_mapped(&s14, &m, wv << 24, 0, out);
        uint32_t tot = (uint32_t)out[3] + out[4] + out[5];
        if (tot < last) { printf("  W %u total %u < %u\n", wv, tot, last); CHECK(0); }
        if (out[3] < out[4] || out[4] < out[5] || out[3] - out[5] > 1) uneven++;
        if (tot % 3) fine++;                                               /* steps a single output couldn't make */
        uint32_t want = (l16[wv] * 3u + 128u) >> 8;
        if (tot != want) { printf("  W %u total %u want %u\n", wv, tot, want); CHECK(0); }
        last = tot;
    }
    CHECK(uneven == 0 && fine > 50 && last == 765);

    /* one W output and no limit: exactly the plain path */
    static uint8_t l8[256];
    gamma_lut(l8, 220);
    out_map_t p = { .intensity16 = 200 << 8, .gamma = l8, .gamma16 = l16, .wmax = 256 };
    for (uint32_t wv = 0; wv < 256; wv += 5) {
        pixel_t px = (wv << 24) | 0x123456u;
        wire_encode_mapped(&one, &p, px, 7, out);
        wire_encode(&one, out_map_px(&p, px, 7), ref);
        CHECK(!memcmp(out, ref, one.n));
    }

    /* white limit: 50% halves the total; 0 turns W off; RGB unaffected */
    m.gamma16 = id16; m.wmax = 128;
    wire_encode_mapped(&s14, &m, 0xFF808080u, 0, out);
    CHECK(out[3] + out[4] + out[5] == 383 && out[0] == 0x80);             /* 765 / 2, rounded */
    m.wmax = 0;
    wire_encode_mapped(&s14, &m, 0xFF808080u, 0, out);
    CHECK(out[3] == 0 && out[4] == 0 && out[5] == 0 && out[1] == 0x80);
    m.wmax = 128;
    wire_encode_mapped(&one, &m, 0xFF000000u, 0, out);
    CHECK(out[3] == 128);                                                 /* single W: limit works too */

    /* dither: the cycle average of the total is the exact value; black never lit */
    m.gamma16 = l16; m.wmax = 256; m.dither = 4;
    for (uint32_t wv = 0; wv < 256; wv += 3) {
        uint32_t sum = 0;
        for (uint32_t f = 0; f < 16; f++) {
            m.frame = f;
            wire_encode_mapped(&s14, &m, wv << 24, 2, out);
            sum += (uint32_t)out[3] + out[4] + out[5];
            if (!wv && (out[3] | out[4] | out[5])) { puts("  black W lit"); CHECK(0); }
        }
        int64_t err = (int64_t)sum * 256 - (int64_t)l16[wv] * 3 * 16;
        if (llabs(err) > 16 * 8) { printf("  W %u dither avg off (%lld)\n", wv, (long long)err); CHECK(0); }
    }
    /* no W in the wire: nothing changes */
    m.dither = 0; m.wmax = 0;
    wire_encode_mapped(&rgbw, &m, 0xFF112233u, 0, out);
    CHECK(out[0] == 0x11 && out[1] == 0x22 && out[2] == 0x33);
}

/* White modes: extract / boost the white contained in RGB, with a white point and the white limit. */
static void t_wire_wmode(void)
{
    static uint16_t id16[256];
    gamma_lut16(id16, GAMMA_OFF);
    wire_t one, s14, rgb; char norm[WIRE_STR_MAX]; uint8_t out[WIRE_MAX];
    CHECK(!wire_parse("RGBW", &one, norm) && !wire_parse("BRG WWW", &s14, norm) && !wire_parse("RGB", &rgb, norm));
    out_map_t m = { .intensity16 = OUT_ONE, .gamma16 = id16, .wmax = 256, .wmode = WMODE_EXTRACT, .wf = { 256, 256, 256 } };

    /* neutral white point: grey moves entirely to W, colors keep their non-white part */
    wire_encode_mapped(&one, &m, 0x00808080u, 0, out);
    CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0x80);
    wire_encode_mapped(&one, &m, 0x00FF8040u, 0, out);
    CHECK(out[0] == 0xBF && out[1] == 0x40 && out[2] == 0 && out[3] == 0x40);
    wire_encode_mapped(&one, &m, 0x00FF0000u, 0, out);                  /* no white in pure red */
    CHECK(out[0] == 0xFF && out[1] == 0 && out[2] == 0 && out[3] == 0);
    wire_encode_mapped(&one, &m, 0x20808080u, 0, out);                  /* program W adds to the extracted white */
    CHECK(out[3] == 0xA0 && out[0] == 0);
    wire_encode_mapped(&one, &m, 0, 0, out);
    CHECK(!out[0] && !out[1] && !out[2] && !out[3]);

    /* a warm white point: exactly that color becomes W; the white limit leaves the rest in RGB */
    m.wf[0] = 256; m.wf[1] = (0xB4 * 256 + 127) / 255; m.wf[2] = (0x6B * 256 + 127) / 255;
    wire_encode_mapped(&one, &m, 0x00FFB46Bu, 0, out);
    CHECK(out[0] <= 1 && out[1] <= 1 && out[2] <= 1 && out[3] == 255);
    m.wmax = 128;
    wire_encode_mapped(&one, &m, 0x00FFB46Bu, 0, out);
    CHECK(out[3] == 128 && out[0] >= 126 && out[0] <= 128 && out[1] >= 88 && out[1] <= 91);
    m.wmax = 256;

    /* W brighter than the RGB it matches (ratio 2): half as much W does the same */
    m.wf[0] = m.wf[1] = m.wf[2] = 512;
    wire_encode_mapped(&one, &m, 0x00808080u, 0, out);
    CHECK(out[0] == 0 && out[3] == 0x40);

    /* boost keeps RGB and adds the white; spread over three outputs */
    m.wf[0] = m.wf[1] = m.wf[2] = 256; m.wmode = WMODE_BOOST;
    wire_encode_mapped(&s14, &m, 0x00808080u, 0, out);
    CHECK(out[0] == 0x80 && out[1] == 0x80 && out[2] == 0x80 && out[3] + out[4] + out[5] == 3 * 0x80);

    /* direct and wires without W: unchanged */
    m.wmode = WMODE_DIRECT;
    wire_encode_mapped(&one, &m, 0x00808080u, 0, out);
    CHECK(out[0] == 0x80 && out[3] == 0);
    m.wmode = WMODE_EXTRACT;
    wire_encode_mapped(&rgb, &m, 0x00808080u, 0, out);
    CHECK(out[0] == 0x80 && out[1] == 0x80 && out[2] == 0x80);

    /* extract preserves the light: RGB + W x white point == input, within rounding, over many colors */
    int bad = 0;
    for (uint32_t c = 0; c < 0x1000000u; c += 0x010307u) {
        wire_encode_mapped(&one, &m, c, 0, out);
        for (int k = 0; k < 3; k++) {
            int in = (int)((c >> (16 - 8 * k)) & 0xFF), back = out[k] + out[3];
            if (abs(in - back) > 1) bad++;
        }
        if (out[0] && out[1] && out[2]) bad++;                            /* some channel must be emptied */
    }
    CHECK(bad == 0);
}

/* 16-bit output intensity: whole Q8 values give exactly the old bytes, values in between round correctly, the dither
 * average follows them, slow fades near black take many more steps, and global x channel is exact. */
static void t_intensity16(void)
{
    /* px_scale16 against the per-byte reference; whole Q8 values = px_scale */
    int bad = 0;
    for (uint32_t k = 0; k <= OUT_ONE; k += (k & 0xFF) ? 13 : 1) {
        for (uint32_t x = 0; x < 256; x++) {
            pixel_t p = x << 24 | (255 - x) << 16 | (x * 7 & 0xFF) << 8 | (x ^ 0x5A), q = px_scale16(p, k), ref = 0;
            for (unsigned sh = 0; sh < 32; sh += 8) ref |= ((((p >> sh) & 0xFFu) * k + 32768u) >> 16) << sh;
            if (q != ref) bad++;
            if (!(k & 0xFF) && k < OUT_ONE && q != px_scale(p, k >> 8)) bad++;
        }
    }
    CHECK(bad == 0);
    /* out_map_px without dither: a whole Q8 intensity gives the old result; in between it lies between its neighbours */
    static uint8_t l8[256]; static uint16_t l16[256];
    gamma_lut(l8, 220); gamma_lut16(l16, 220);
    out_map_t m = { .intensity16 = 77 << 8, .gamma = l8, .gamma16 = l16 };
    CHECK(out_map_px(&m, 0x00C08040u, 0) == px_scale(px_lut(0x00C08040u, l8), 77));
    int order = 0;
    for (uint32_t k = 1000; k < 1300; k++) {
        out_map_t a = m, b = m; a.intensity16 = k; b.intensity16 = k + 1;
        for (uint32_t x = 0; x < 256; x += 15) { pixel_t p = x * 0x01010101u; order += (out_map_px(&a, p, 0) & 0xFF) > (out_map_px(&b, p, 0) & 0xFF); }
    }
    CHECK(order == 0);
    /* dither: the cycle average follows a fine intensity (e.g. 3.5/256 near black) */
    for (unsigned bits = 1; bits <= DITHER_MAX; bits++) {
        unsigned n = 1u << bits;
        uint32_t ks[] = { 300, 896, 1000, 12345, 40000, 65535 };
        for (unsigned j = 0; j < sizeof ks / sizeof ks[0]; j++) {
            out_map_t d = { .intensity16 = ks[j], .gamma16 = l16, .dither = (uint8_t)bits };
            for (int b = 0; b < 256; b += 3) {
                uint32_t v = (l16[b] * ks[j]) >> 16, sum = 0;
                for (uint32_t f = 0; f < n; f++) { d.frame = f; sum += out_map_px(&d, (pixel_t)b * 0x01010101u, 5) & 0xFF; }
                int64_t err = (int64_t)sum * 256 - (int64_t)v * n;
                if (llabs(err) > (int64_t)n * (128 >> bits)) { printf("  avg off: bits %u k %u b %d\n", bits, ks[j], b); CHECK(0); }
            }
        }
    }
    /* W outputs (wire.h) take the same fine intensity: one W and no limit = the plain path */
    wire_t one; char norm[WIRE_STR_MAX]; uint8_t out[WIRE_MAX], ref[WIRE_MAX];
    CHECK(!wire_parse("GRBW", &one, norm));
    out_map_t w = { .intensity16 = 12345, .gamma = l8, .gamma16 = l16, .wmax = 256 };
    for (uint32_t wv = 0; wv < 256; wv += 5) {
        pixel_t px = (wv << 24) | 0x80C0FFu;
        wire_encode_mapped(&one, &w, px, 3, out); wire_encode(&one, out_map_px(&w, px, 3), ref);
        CHECK(!memcmp(out, ref, one.n));
    }
    /* global x channel */
    CHECK(intensity16_mul(OUT_ONE, 12345) == 12345 && intensity16_mul(12345, OUT_ONE) == 12345 && intensity16_mul(OUT_ONE, OUT_ONE) == OUT_ONE);
    CHECK(intensity16_mul(128 << 8, 3 << 8) == 384 && intensity16_mul(0, OUT_ONE) == 0);
    /* a minute-long fade from 4/256 to black: 4 steps at 0..256, hundreds at output precision, monotonic, arrives */
    fader_t f = { 0 };
    fader_set(&f, 4, 0);
    CHECK(fader_value16(&f) == 4 << 8);
    fader_set(&f, 0, 3600);
    uint32_t last16 = fader_value16(&f), changes16 = 0, changes8 = 0, last8 = fader_value(&f), mono = 1;
    for (int i = 0; i < 3600; i++) {
        fader_step(&f);
        uint32_t v = fader_value16(&f), v8 = fader_value(&f);
        if (v > last16) mono = 0;
        changes16 += v != last16; changes8 += v8 != last8;
        last16 = v; last8 = v8;
    }
    CHECK(mono && last16 == 0 && changes8 <= 4 && changes16 >= 1000);
    fader_set(&f, 200, 0);
    CHECK(fader_value16(&f) == 200 << 8 && fader_value(&f) == 200);
}

/* Intensity faders: at once, a fade reaches its target after the given frames, hour-long fades still move, retarget. */
static void t_fader(void)
{
    fader_t f = { 0 };
    fader_set(&f, 256, 0);
    CHECK(fader_value(&f) == 256 && fader_target(&f) == 256);
    fader_set(&f, 0, 120);                               /* 2 s at 60 Hz */
    int frames = 0;
    uint16_t last = 256, mid = 0;
    while (fader_value(&f) != 0 && frames < 1000) {
        fader_step(&f); frames++;
        if (fader_value(&f) > last) { CHECK(0); break; }   /* monotonic */
        last = fader_value(&f);
        if (frames == 60) mid = fader_value(&f);
    }
    CHECK(frames >= 119 && frames <= 121 && mid >= 126 && mid <= 130 && f.cur == f.target);
    fader_set(&f, 1, 216000);                            /* an hour for one step: still moves, and arrives */
    int32_t before = f.cur;
    fader_step(&f);
    CHECK(f.cur > before);
    for (int i = 0; i < 216000; i++) fader_step(&f);
    CHECK(fader_value(&f) == 1 && f.cur == f.target);
    fader_set(&f, 200, 100);                              /* retarget mid-fade: from where it is */
    for (int i = 0; i < 50; i++) fader_step(&f);
    uint16_t half = fader_value(&f);
    fader_set(&f, 0, 10);
    CHECK(half > 90 && half < 110);
    for (int i = 0; i < 10; i++) fader_step(&f);
    CHECK(fader_value(&f) == 0);
    for (int i = 0; i < 5; i++) fader_step(&f);           /* stays put */
    CHECK(fader_value(&f) == 0 && fader_target(&f) == 0);
}

/* Automation: context rules, the host hooks, compile-time library check. */
static struct { int plays, stops, sets; int last_ch; char last_name[16]; uint16_t inten[4]; uint32_t last_fade; } fh;
static int fh_n(void) { return 2; }
static uint16_t fh_get(int ch) { return fh.inten[ch < 0 ? 3 : ch]; }
static void fh_set(int ch, uint16_t v, uint32_t fade) { fh.inten[ch < 0 ? 3 : ch] = v; fh.sets++; fh.last_fade = fade; fh.last_ch = ch; }
static const char *fh_play(int ch, const char *name) { fh.plays++; fh.last_ch = ch; snprintf(fh.last_name, sizeof fh.last_name, "%s", name); return strcmp(name, "broken") ? NULL : "line 1 col 1: bad"; }
static const char *fh_stop(int ch) { fh.stops++; fh.last_ch = ch; return NULL; }
static bool fh_exists(const char *name) { return !strcmp(name, "sunset") || !strcmp(name, "chase") || !strcmp(name, "broken"); }

/* Fake GPIO outputs: 0 = pump (binary), 2 = fan, 1 unused. */
static uint16_t fio[3];
static int fio_find(const char *n) { return !strcmp(n, "pump") ? 0 : !strcmp(n, "fan") ? 2 : -1; }
static bool fio_used(int n) { return n == 0 || n == 2; }
static uint16_t fio_get(int n) { return fio[n]; }
static const char *fio_set(int n, uint16_t v) { fio[n] = n == 0 ? (v ? 256 : 0) : v; return NULL; }

static void t_io(channel_t *ch)
{
    char e[DSL_ERR_LEN];
    static const dsl_host_t host = { fh_n, fh_get, fh_set, fh_play, fh_stop, fh_exists, fio_find, fio_used, fio_get, fio_set, NULL, NULL, NULL, NULL, NULL };
    CHECK(channel_set_source(ch, "(io-set 0 t)", e, sizeof e) != 0 && strstr(e, "only works in the automation program"));
    ch->ctx = DSL_CTX_AUTOMATION;
    dsl_set_host(NULL);
    CHECK(channel_set_source(ch, "(io-set pump t)", e, sizeof e) != 0 && strstr(e, "no output by that name"));   /* no host: no names */
    CHECK(run(ch, "(io-set 0 t)", e) == 0 && ch->perf.faults == 1);       /* no host: a fault */
    dsl_set_host(&host);
    memset(fio, 0, sizeof fio);
    uint32_t f0 = ch->perf.faults;
    CHECK(run(ch, "(io-set pump t) (io-set 2 0.5)", e) == 0 && fio[0] == 256 && fio[2] == 128 && ch->perf.faults == f0);
    CHECK(run(ch, "(io-set fan (if (eq (io pump) 1) 0.25 0))", e) == 0 && fio[2] == 64);
    CHECK(run(ch, "(io-set pump nil) (io-set fan 1)", e) == 0 && fio[0] == 0 && fio[2] == 256);
    CHECK(run(ch, "(io-set fan 0.001)", e) == 0 && fio[2] == 1);          /* a tiny level is still on */
    CHECK(run(ch, "(io-set fan 0)", e) == 0 && fio[2] == 0);
    CHECK(run(ch, "(io-set 0 (ge (env hour) 0))", e) == 0 && fio[0] == 256); /* a run-time bool */
    CHECK(channel_set_source(ch, "(io-set pmup t)", e, sizeof e) != 0 && strstr(e, "no output by that name"));
    CHECK(channel_set_source(ch, "(io-set 1 t)", e, sizeof e) != 0 && strstr(e, "no such output"));      /* unused slot */
    CHECK(channel_set_source(ch, "(io-set 0 #FF0000)", e, sizeof e) != 0 && strstr(e, "level 0..1"));
    CHECK(channel_set_source(ch, "(io-set 0 (stop 0))", e, sizeof e) != 0);
    CHECK(run(ch, "(def k 2) (io-set k 0.5)", e) == 0 && fio[2] == 128);    /* a variable is a value, not a name */
    CHECK(run(ch, "(io-set (+ 1 1) 0.75)", e) == 0 && fio[2] == 192);
    CHECK(run(ch, "(io-set (env hour) 1)", e) == 0);                         /* run-time slot */
    dsl_set_host(NULL);
    ch->ctx = DSL_CTX_RENDER;
}

/* inputs: slot 4 "button" (digital), 5 "knob" (analog); fin_mode lets a test change a slot's mode behind a program */
static bool     fin_button;
static uint32_t fin_knob;
static int      fin_samples, fin_mode4 = DSL_IO_INPUT;
static int  fin_find(const char *n) { return !strcmp(n, "button") ? 4 : !strcmp(n, "knob") ? 5 : fio_find(n); }
static bool fin_used(int n) { return fio_used(n) || n == 4 || n == 5; }
static int  fin_mode(int n) { return n == 0 ? DSL_IO_BINARY : n == 2 ? DSL_IO_PWM : n == 4 ? fin_mode4 : n == 5 ? DSL_IO_ANALOG : -1; }
static bool fin_get(int n) { return n == 4 && fin_button; }
static uint32_t fin_analog(int n, int samples) { fin_samples = samples; return n == 5 ? fin_knob : 0; }

static void t_inputs(channel_t *ch)
{
    char e[DSL_ERR_LEN];
    static const dsl_host_t host = { fh_n, fh_get, fh_set, fh_play, fh_stop, fh_exists, fin_find, fin_used, fio_get, fio_set,
                                     NULL, NULL, fin_mode, fin_get, fin_analog };
    dsl_set_host(NULL);
    CHECK(channel_set_source(ch, "(in button)", e, sizeof e) != 0 && strstr(e, "no input by that name"));   /* no host: no names */
    ch->perf.faults = 0;
    CHECK(run(ch, "(if (in 0) (fill (buf O) #FFFFFF))", e) == 0 && ch->perf.faults == 1 && strstr(ch->perf.last_err, "inputs aren't available"));
    dsl_set_host(&host);
    ch->perf.faults = 0;
    /* channel programs read inputs (not automation-only) */
    fin_button = true;
    CHECK(dsl_true(ch, "(in button)") && dsl_true(ch, "(in 4)"));
    fin_button = false;
    CHECK(!dsl_true(ch, "(in button)") && dsl_true(ch, "(not (in button))"));
    fin_knob = 32768;
    CHECK(dsl_true(ch, "(eq (analog knob) 0.5)") && fin_samples == DSL_ANALOG_SAMPLES_DEFAULT);
    CHECK(dsl_true(ch, "(eq (analog knob 64) 0.5)") && fin_samples == 64);
    CHECK(dsl_true(ch, "(eq (analog knob _) 0.5)") && fin_samples == DSL_ANALOG_SAMPLES_DEFAULT);
    CHECK(dsl_true(ch, "(eq (analog 5 1) 0.5)") && fin_samples == 1);
    fin_knob = 65536;
    CHECK(dsl_true(ch, "(eq (analog knob 256) 1)") && fin_samples == 256);
    fin_knob = 70000;                                                    /* a host out of range is clamped to 1 */
    CHECK(dsl_true(ch, "(eq (analog knob) 1)"));
    fin_knob = 0;
    CHECK(dsl_true(ch, "(eq (analog knob) 0)"));
    env = (env_t){ .second = 0, .intensity = 256 };
    CHECK(dsl_true(ch, "(eq (analog knob (env second)) 0)") && fin_samples == 1);   /* run time: clamped to 1..256 */
    env.second = 59;
    CHECK(run(ch, "(def k (* (env second) 10)) (if (eq (analog knob k) 0) (fill (buf O) #FFFFFF))", e) == 0 && fin_samples == 256);
    env = (env_t){ .intensity = 256 };
    fin_knob = 16384;                                                    /* scale a bar with a knob */
    CHECK(run(ch, "(fill (buf O) #000000) (fill (buf O) #FF0000 0 (round (* (analog knob) (env npix))))", e) == 0);
    CHECK(ch->buf[BUF_O][1] == 0xFF0000 && ch->buf[BUF_O][3] == 0);      /* 10 pixels * 0.25 = 2.5 -> 3 */
    CHECK(ch->perf.faults == 0);
    /* compile errors: names, kinds, samples */
    struct { const char *src, *want; } bad[] = {
        { "(in buton)", "no input by that name" },             { "(analog knb)", "no analog input by that name" },
        { "(in knob)", "that is an analog input, not an input" }, { "(analog button)", "that is a digital input, not an analog input" },
        { "(in pump)", "that is an output, not an input" },      { "(analog fan)", "that is an output, not an analog input" },
        { "(in 1)", "no such input" },                           { "(analog knob 0)", "samples must be 1..256" },
        { "(analog knob 257)", "samples must be 1..256" },       { "(analog knob 0.5)", NULL },
        { "(in)", NULL },                                        { "(in button 1)", NULL },
        { "(if (in button 1) 0)", NULL },                        { "(fill (buf O) (in button))", NULL },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        bool failed = channel_set_source(ch, bad[i].src, e, sizeof e) != 0;
        CHECK(failed && (!bad[i].want || strstr(e, bad[i].want)));
        if (!failed || (bad[i].want && !strstr(e, bad[i].want))) printf("  %s -> %s\n", bad[i].src, failed ? e : "(compiled)");
    }
    /* outputs refuse inputs, at compile time and (when the slot changes later) at run time */
    ch->ctx = DSL_CTX_AUTOMATION;
    CHECK(channel_set_source(ch, "(io-set button t)", e, sizeof e) != 0 && strstr(e, "that is a digital input: read it with (in x)"));
    CHECK(channel_set_source(ch, "(io knob)", e, sizeof e) != 0 && strstr(e, "that is an analog input"));
    CHECK(run(ch, "(if (in button) (io-set pump t) (io-set pump nil))", e) == 0 && fio[0] == 0);   /* automation reads inputs too */
    fin_button = true;
    CHECK(run(ch, "(if (in button) (io-set pump t) (io-set pump nil))", e) == 0 && fio[0] == 256);
    ch->ctx = DSL_CTX_RENDER;
    CHECK(channel_set_source(ch, "(if (in button) (fill (buf O) #FFFFFF))", e, sizeof e) == 0);
    fin_mode4 = DSL_IO_BINARY;                                           /* slot 4 became an output after compiling */
    ch->perf.faults = 0;
    channel_render(ch);
    CHECK(ch->perf.faults == 1 && strstr(ch->perf.last_err, "not a digital input now"));
    fin_mode4 = DSL_IO_INPUT;
    ch->perf.faults = 0;
    /* a host that doesn't know modes (the IDE offline): names resolve, kinds aren't checked */
    static const dsl_host_t loose = { .io_find = fin_find, .io_used = fin_used, .in_get = fin_get, .analog_get = fin_analog };
    dsl_set_host(&loose);
    CHECK(channel_set_source(ch, "(if (in knob) (fill (buf O) #FFFFFF))", e, sizeof e) == 0);
    CHECK(dsl_true(ch, "(not (in knob))") && ch->perf.faults == 0);
    dsl_set_host(NULL);
}

static void t_automation(channel_t *ch)
{
    char e[DSL_ERR_LEN];
    static const dsl_host_t host = { fh_n, fh_get, fh_set, fh_play, fh_stop, fh_exists, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
    /* render programs: automation natives are a compile error */
    CHECK(channel_set_source(ch, "(play 0 sunset)", e, sizeof e) != 0 && strstr(e, "only works in the automation program"));
    CHECK(channel_set_source(ch, "(fill (buf O) #FF0000) (set-intensity 0 0.5)", e, sizeof e) != 0);

    ch->ctx = DSL_CTX_AUTOMATION;
    dsl_set_host(NULL);
    CHECK(run(ch, "(stop 0)", e) == 0 && ch->perf.faults == 1);          /* no host: a fault, not a crash */
    dsl_set_host(&host);
    memset(&fh, 0, sizeof fh);
    fh.inten[0] = 256; fh.inten[3] = 128;
    /* only G buffers */
    CHECK(channel_set_source(ch, "(fill (buf O) #FF0000)", e, sizeof e) != 0 && strstr(e, "only use G0..G7"));
    CHECK(channel_set_source(ch, "(fill (buf x) #FF0000)", e, sizeof e) != 0);
    CHECK(run(ch, "(fill (buf G3) #123456 0 2)", e) == 0 && globals.buf[3][1] == 0x123456);
    /* library names checked at compile time */
    CHECK(channel_set_source(ch, "(play 0 nosuch)", e, sizeof e) != 0 && strstr(e, "no library program"));
    CHECK(channel_set_source(ch, "(play 0 Sunset)", e, sizeof e) != 0 && strstr(e, "lower case"));
    uint32_t f0 = ch->perf.faults;
    CHECK(run(ch, "(play 1 chase) (stop 0)", e) == 0);
    CHECK(fh.plays == 1 && !strcmp(fh.last_name, "chase") && fh.stops == 1 && fh.last_ch == 0 && ch->perf.faults == f0);
    CHECK(run(ch, "(play 5 chase)", e) == 0 && ch->perf.faults == f0 + 1);           /* no such channel: fault */
    CHECK(run(ch, "(play 0 broken)", e) == 0 && ch->perf.faults == f0 + 2);          /* play failed: fault */
    /* intensity: read 0..1, set with _ = global, fade in seconds */
    CHECK(run(ch, "(if (eq (intensity) 0.5) (set-intensity 1 0.25 2.5))", e) == 0);
    CHECK(fh.inten[1] == 64 && fh.last_fade == 2500 && fh.last_ch == 1);
    CHECK(run(ch, "(set-intensity _ (* (intensity 0) 0.5))", e) == 0 && fh.inten[3] == 128 && fh.last_ch == -1 && fh.last_fade == 0);
    CHECK(run(ch, "(set-intensity 0 1 (env hour))", e) == 0 && fh.inten[0] == 256);      /* int fade from env */
    CHECK(channel_set_source(ch, "(set-intensity _ _)", e, sizeof e) != 0);          /* the value can't be _ */
    CHECK(channel_set_source(ch, "(play _ sunset)", e, sizeof e) != 0);              /* only set-intensity takes _ first */
    CHECK(run(ch, "(set-intensity 0 0.5 -1)", e) == 0 && ch->perf.faults == f0 + 3);  /* negative fade: fault */
    CHECK(run(ch, "(if (ge (env hour) 18) (play 0 sunset) (stop 0))", e) == 0);
    dsl_set_host(NULL);
    ch->ctx = DSL_CTX_RENDER;
}

/* Run `defs` then a boolean `cond`: O[0] turns green when it holds. */
static bool holds(channel_t *ch, const char *defs, const char *cond)
{
    char src[1400], e[DSL_ERR_LEN] = "";
    snprintf(src, sizeof src, "%s (if %s (fill (buf O) #00FF00) (fill (buf O) #FF0000))", defs, cond);
    uint32_t f0 = ch->perf.faults;
    if (run(ch, src, e) != 0) { printf("  compile: %s\n  src=%s\n", e, src); return false; }
    if (ch->perf.faults != f0) { printf("  fault: %s\n  src=%s\n", ch->perf.last_err, src); return false; }
    return ch->buf[BUF_O][0] == 0x00FF00;
}
static void expect_err(channel_t *ch, const char *src, const char *want)
{
    char e[DSL_ERR_LEN] = "";
    CHECK(channel_set_source(ch, src, e, sizeof e) != 0);
    if (!strstr(e, want)) { printf("  src=%s\n  got='%s' want='%s'\n", src, e, want); CHECK(0); }
}

static int64_t g_now;
static int64_t fake_clock(void) { return g_now++; }

static void t_functions(channel_t *ch)
{
    char e[DSL_ERR_LEN];
    /* calls, results, defined after use, params of every type */
    CHECK(holds(ch, "(defn sq (x:int) (* x x))", "(eq (+ (sq 2) (sq 3)) 13)"));
    CHECK(holds(ch, "(defn sq (x:int) (* x x))", "(eq (sq (sq 2)) 16)"));                   /* inner call first */
    CHECK(holds(ch, "(def r (twice 4)) (defn twice (x:int) (* 2 x))", "(eq r 8)"));          /* use before the defn */
    CHECK(holds(ch, "(defn half (x:decimal) (/ x 2))", "(eq (half 3) 1.5)"));                /* int -> decimal param */
    CHECK(holds(ch, "(defn pick (b:bool c:color d:color) (if b c d))", "(eq (pick nil #FF0000 #0000FF) #0000FF)"));
    CHECK(holds(ch, "(defn n (s:str) (str-len s))", "(eq (n \"abc\") 3)"));
    CHECK(holds(ch, "(defn len (b:buf) (- (str-len (str-num 5)) 1))", "(eq (len (buf O)) 0)"));
    CHECK(holds(ch, "(defn one () 1)", "(eq (one) 1)"));
    CHECK(holds(ch, "(defn id (c:color) c)", "(eq (id #123456) #123456)"));                   /* a value as the body */
    CHECK(holds(ch, "(defn k () #FF0000)", "(eq (k) #FF0000)"));                              /* a constant body */
    /* several forms, locals, a parameter reassigned; a statement function */
    CHECK(holds(ch, "(defn f (x:int) (def y (* x 10)) (def x (+ x 1)) (+ x y))", "(eq (f 2) 23)"));
    CHECK(run(ch, "(fill (buf O) #000000) (defn paint (b:buf n:int) (fill b #0000FF 0 n)) (paint (buf O) 3)", e) == 0);
    CHECK(ch->buf[BUF_O][2] == 0xFF && ch->buf[BUF_O][3] == 0);
    CHECK(holds(ch, "(defn g (x:int) (twice (twice x))) (defn twice (x:int) (* 2 x))", "(eq (g 3) 12)"));   /* callee later */

    /* self tail calls are loops */
    CHECK(holds(ch, "(defn wrap (x:int n:int) (if (ge x n) (wrap (- x n) n) x))", "(eq (wrap 23 5) 3)"));
    CHECK(holds(ch, "(defn steps (n:int k:int) (if (eq n 1) k (if (eq (mod n 2) 0) (steps (/ n 2) (+ k 1)) "
                    "(steps (+ (* 3 n) 1) (+ k 1)))))", "(eq (steps 27 0) 111)"));
    CHECK(holds(ch, "(defn dec (x:decimal) (if (gt x 1.0) (dec (- x 1)) x))", "(eq (dec 3.5) 0.5)"));
    CHECK(holds(ch, "(defn sum (i:int acc:int) (if (gt i 0) (sum (- i 1) (+ acc i)) acc))", "(eq (sum 100 0) 5050)"));
    CHECK(holds(ch, "(defn swap (a:int b:int n:int) (if (gt n 0) (swap b a (- n 1)) (- a b)))", "(eq (swap 1 2 3) 1)"));  /* new values read the old */
    CHECK(run(ch, "(fill (buf O) #000000) (defn dots (i:int) (set (buf O) i #FFFFFF) (if (lt i 8) (dots (+ i 2)))) (dots 0)", e) == 0);
    for (int i = 0; i < 10; i++) CHECK(ch->buf[BUF_O][i] == (i % 2 == 0 ? 0xFFFFFFu : 0u));
    uint32_t f0 = ch->perf.faults;                                       /* an endless loop hits the node budget */
    CHECK(run(ch, "(defn spin (x:int) (if t (spin x) 0)) (def z (spin 1))", e) == 0);
    CHECK(ch->perf.faults == f0 + 1 && strstr(ch->perf.last_err, "budget"));
    channel_render(ch);
    CHECK(ch->perf.faults == f0 + 2);                                    /* the next run starts afresh (tail flag) */

    /* map-px */
    CHECK(run(ch, "(fill (buf O) #102030) (map-px (buf O) (buf O) (fn (c:color) (lerp #000000 c 0.5)))", e) == 0);
    for (int i = 0; i < 10; i++) CHECK(ch->buf[BUF_O][i] == 0x081018);
    CHECK(run(ch, "(map-px (buf O) (buf O) (fn (c:color i:int) (if (lt i 3) #FF0000 c)) 1 -2)", e) == 0);
    CHECK(ch->buf[BUF_O][0] == 0x081018 && ch->buf[BUF_O][1] == 0xFF0000 && ch->buf[BUF_O][2] == 0xFF0000 && ch->buf[BUF_O][3] == 0x081018
          && ch->buf[BUF_O][8] == 0x081018);
    for (int i = 0; i < 10; i++) ch->buf[BUF_X][i] = (uint32_t)i;
    CHECK(run(ch, "(defn plus (c:color i:int) (if (eq c #000003) #FFFFFF c)) (fill (buf O) #000000) (map-px (slice (buf O) 0 _ 2) (buf X) plus)", e) == 0);
    for (int i = 0; i < 10; i++) CHECK(ch->buf[BUF_O][i] == (i % 2 ? 0u : i == 6 ? 0xFFFFFFu : (uint32_t)i / 2));   /* stride 2 dst */
    CHECK(run(ch, "(map-px (buf X) (slice (buf X) _ _ -1) (fn (c:color) c))", e) == 0);    /* reversed onto itself: staged */
    for (int i = 0; i < 10; i++) CHECK(ch->buf[BUF_X][i] == (uint32_t)(9 - i));
    for (int i = 0; i < 10; i++) ch->buf[BUF_X][i] = (uint32_t)i;
    CHECK(run(ch, "(map-px (slice (buf X) 1) (buf X) (fn (c:color) c))", e) == 0);          /* shifted overlap: src first */
    for (int i = 0; i < 10; i++) CHECK(ch->buf[BUF_X][i] == (uint32_t)(i ? i - 1 : 0));
    for (int i = 0; i < 10; i++) ch->buf[BUF_X][i] = (uint32_t)i;
    CHECK(run(ch, "(map-px (buf X) (buf X) (fn (c:color i:int) (if (gt i 0) (get (buf X) (- i 1)) c)))", e) == 0);
    for (int i = 0; i < 10; i++) CHECK(ch->buf[BUF_X][i] == 0);          /* same view: f sees what it already wrote */
    CHECK(run(ch, "(fill (buf O) #000000) (map-px (buf O) (buf O) (fn (c:color) #FFFFFF) 5 5)", e) == 0);   /* empty range */
    CHECK(ch->buf[BUF_O][5] == 0);
    CHECK(holds(ch, "(defn dim (b:buf k:decimal) (map-px b b (fn (c:color) (lerp #000000 c k)))) (fill (buf Y) #808080) (dim (buf Y) 0.5)",
                "(eq (get (buf Y) 9) #404040)"));                                       /* fn sees the defn's params */
    CHECK(holds(ch, "(def k 0.25) (fill (buf Y) #808080) (map-px (buf Y) (buf Y) (fn (c:color) (lerp #000000 c k)))",
                "(eq (get (buf Y) 0) #202020)"));                                       /* and top-level variables */

    /* repeat */
    CHECK(holds(ch, "(def n 0) (repeat 5 (fn (i:int) (def n (+ n i))))", "(eq n 10)"));  /* writes the outer variable */
    CHECK(holds(ch, "(def n 0) (repeat 3 (fn () (def n (+ n 1))))", "(eq n 3)"));
    CHECK(holds(ch, "(def n 7) (repeat 0 (fn () (def n 0))) (repeat -3 (fn () (def n 0)))", "(eq n 7)"));
    CHECK(holds(ch, "(def n 0) (repeat 3 (fn (i:int) (def m (* i 2)) (def n (+ n m))))", "(eq n 6)"));   /* m is local */
    CHECK(holds(ch, "(defn tsum (k:int) (def s 0) (repeat k (fn (i:int) (def s (+ s i 1)))) s)", "(eq (tsum 4) 10)"));
    CHECK(holds(ch, "(def n 0) (repeat 3 (fn (i:int) (repeat 4 (fn (j:int) (def n (+ n 1))))))", "(eq n 12)"));
    CHECK(run(ch, "(fill (buf O) #000000) (repeat 3 (fn (i:int) (set (buf O) (* i 3) #FF0000)))", e) == 0);
    for (int i = 0; i < 10; i++) CHECK(ch->buf[BUF_O][i] == (i % 3 == 0 && i < 9 ? 0xFF0000u : 0u));
    f0 = ch->perf.faults;
    CHECK(run(ch, "(repeat 1000000 (fn () (fill (buf O) #000000)))", e) == 0);
    CHECK(ch->perf.faults == f0 + 1 && strstr(ch->perf.last_err, "budget"));

    /* scratch used by one call is given back, unless a string goes into an enclosing variable */
    channel_t small;
    if (channel_init(&small, 10, 256, &env, &globals) == 0) {
        CHECK(run(&small, "(repeat 100 (fn (i:int) (def s (str-num i 30))))", e) == 0 && small.perf.faults == 0);
        CHECK(run(&small, "(map-px (buf O) (buf O) (fn (c:color) (def s (str-num 1 32)) c))", e) == 0 && small.perf.faults == 0);
        CHECK(run(&small, "(def s \"\") (repeat 100 (fn (i:int) (def s (str-num i 30))))", e) == 0);
        CHECK(small.perf.faults == 1 && strstr(small.perf.last_err, "scratch"));
        CHECK(holds(&small, "(def s \"\") (repeat 3 (fn (i:int) (def s (str-cat s \"ab\"))))", "(eq (str-len s) 6)"));
        channel_free(&small);
    } else CHECK(0);

    /* errors */
    static const struct { const char *src, *want; } errs[] = {
        { "(defn f (x:int) (+ 1 (f x)))",                       "'f' calls itself here: that only works as its very last step" },
        { "(defn f (x:int) (if t (+ 1 (f x)) 0))",             "calls itself here" },
        { "(defn f (x:int) (if (f x) 1 0))",                   "calls itself here" },          /* the condition isn't a tail */
        { "(defn f (x:int) (f x))",                             "'f' never returns" },
        { "(defn f (x:int) (if t (f 1) (f 2)))",                "'f' never returns" },
        { "(defn a () (b)) (defn b () (a))",                    "'b' and 'a' call each other in a circle" },
        { "(defn a () (b)) (defn b () (c)) (defn c () (a))",    "call each other in a circle" },
        { "(defn a (c:color) (map-px (buf O) (buf O) b) c) (defn b (c:color) (a c))", "call each other in a circle" },
        { "(defn a (c:color) (map-px (buf O) (buf O) a) c)",    "'a' can't pass itself on" },
        { "(defn a () (repeat 2 (fn () (a))) 1)",               "'a' calls itself here" },
        { "(defn f (x:int) x) (defn f (y:int) y)",              "'f' is defined twice" },
        { "(defn fill (x:int) x)",                              "'fill' is a built-in function" },
        { "(defn fn (x:int) x)",                                "bad function name" },
        { "(defn f (x:int))",                                   "'f' needs a body" },
        { "(defn f x)",                                         "'defn' takes a name, a parameter list and a body" },
        { "(defn (x:int) 1)",                                   "'defn' takes a name" },
        { "(defn f (x) x)",                                     "a parameter is name:type" },
        { "(defn f (x:float) x)",                               "unknown parameter type 'float'" },
        { "(defn f (a.b:int) 1)",                               "bad parameter name" },
        { "(defn f (x:int x:int) x)",                           "parameter 'x' appears twice" },
        { "(defn f (a:int b:int c:int d:int e:int f:int g:int h:int i:int j:int k:int) 1)", "too many parameters" },
        { "(defn g (x:int) x) (defn f (g:int) g)",              "'g' is already a function name" },
        { "(defn g (x:int) x) (def g 1)",                       "'g' is already a function name" },
        { "(if t (defn f (x:int) x))",                          "defn only works at the top level" },
        { "(fn (x:int) x)",                                     "fn makes a function to pass to map-px or repeat" },
        { "(def g (fn (x:int) x))",                             "fn makes a function" },
        { "(defn f (x:int) x) (fill (buf O) #FF0000 0 (f))",    "'f' takes 1 argument(s), got 0" },
        { "(defn f (x:int) x) (fill (buf O) #FF0000 0 (f 1.5))", "expected int, got number (use floor or round)" },
        { "(defn f (c:color) c) (fill (buf O) (f 1))",          "expected color, got int" },
        { "(defn f (x:int) (fill (buf O) #FF0000)) (def y (+ (f 1) 1))", "expected number, but 'f' returns buffer" },
        { "(defn f (x:int) (if t (fill (buf O) #FF0000))) (def y (f 1))", "returns nothing" },
        { "(defn f (x:int) x) (def y f)",                       "'f' is a function: call it" },
        { "(def k 2) (defn f (x:int) (* x k))",                 "unknown name 'k'" },            /* defn sees no outer variables */
        { "(map-px (buf O) (buf O) nope)",                      "unknown function 'nope'" },
        { "(map-px (buf O) (buf O) 3)",                         "expected a function" },
        { "(map-px (buf O) (buf O) (fn (c:color) 1))",          "returns a color" },
        { "(map-px (buf O) (buf O) (fn (i:int) #FF0000))",      "(c:color) or (c:color i:int)" },
        { "(map-px (buf O) (buf O) (fn (c:color i:decimal) c))", "(c:color) or (c:color i:int)" },
        { "(defn f (c:color i:int j:int) c) (map-px (buf O) (buf O) f)", "(c:color) or (c:color i:int)" },
        { "(repeat 3 (fn (i:color) 1))",                        "repeat needs a function of (i:int) or ()" },
        { "(repeat 3 (fn (i:int j:int) 1))",                    "repeat needs a function" },
        { "(repeat 3 _)",                                       "'_' can only stand in" },
        { "(repeat 3 (fn x 1))",                                "'fn' takes a parameter list and a body" },
        { "(repeat 3 (fn (i:int)))",                            "'fn' needs a body" },
        { "(def s \"\") (repeat 2 (fn (i:int) (def s 1)))",    "'s' is already string, it can't become int" },
    };
    for (size_t i = 0; i < sizeof errs / sizeof errs[0]; i++) expect_err(ch, errs[i].src, errs[i].want);

    /* the evaluator depth is checked at compile time, across calls */
    static char deep[1400];
    strcpy(deep, "(defn f0 (x:int) (+ 1 (+ 1 (+ 1 (+ 1 (+ 1 (+ 1 (+ 1 (+ 1 x)))))))))");
    for (int i = 1; i < 6; i++) {
        char d[160];
        snprintf(d, sizeof d, "(defn f%d (x:int) (+ 1 (+ 1 (+ 1 (+ 1 (+ 1 (+ 1 (f%d x))))))))", i, i - 1);
        strcat(deep, d);
    }
    expect_err(ch, deep, "calls nested too deep");                       /* f3 is over the limit */
    *strstr(deep, "(defn f3") = 0;
    CHECK(holds(ch, deep, "(eq (f2 0) 20)"));
    /* the limit counts bytes, not levels: wide calls and ifs cost more */
    static char wide[1400];
    char body[400] = "x";
    for (int i = 0; i < 11; i++) {   /* grows to 243 bytes; checking the length also keeps gcc's -Wformat-truncation quiet */
        char t[sizeof body];
        int n = snprintf(t, sizeof t, "(+ 1 1 1 1 1 1 1 1 1 %s)", body);
        CHECK(n > 0 && n < (int)sizeof t);
        memcpy(body, t, sizeof body);
    }
    snprintf(wide, sizeof wide, "(defn g0 (x:int) %s)", body);
    CHECK(holds(ch, wide, "(gt (g0 0) 0)"));                             /* the widest program one function can hold */
    strcat(wide, "(defn g1 (x:int) (g0 x))(defn g2 (x:int) (+ 1 1 1 1 1 1 1 1 1 (+ 1 1 1 1 1 1 1 1 1 (+ 1 1 1 1 1 1 1 1 1 "
                 "(+ 1 1 1 1 1 1 1 1 1 (+ 1 1 1 1 1 1 1 1 1 (+ 1 1 1 1 1 1 1 1 1 (+ 1 1 1 1 1 1 1 1 1 (+ 1 1 1 1 1 1 1 1 1 (g1 x))))))))))");
    expect_err(ch, wide, "calls nested too deep");
    strcpy(wide, "(defn i0 (x:int) (if t (if t (if t (if t (if t (if t (if t (if t (if t (if t x))))))))))) "
                 "(defn i1 (x:int) (if t (if t (if t (if t (if t (if t (if t (if t (if t (if t (i0 x)))))))))))) "
                 "(defn i2 (x:int) (if t (if t (if t (if t (i1 x))))))");
    expect_err(ch, wide, "calls nested too deep");

    /* time limit: only function calls and loops look at the clock (every 32nd) */
    g_now = 0; dsl_set_clock(fake_clock);
    ch->deadline_us = 100;
    f0 = ch->perf.faults;
    CHECK(run(ch, "(fill (buf O) #000000) (fill (buf O) #000000) (scale (buf O) 0.5)", e) == 0 && ch->perf.faults == f0 && g_now == 0);
    CHECK(run(ch, "(repeat 50 (fn () (fill (buf O) #000000)))", e) == 0 && ch->perf.faults == f0);    /* one reading, in time */
    CHECK(g_now == 1);
    g_now = 1000;
    CHECK(run(ch, "(fill (buf O) #000000) (repeat 50 (fn () (fill (buf O) #FF0000)))", e) == 0);
    CHECK(ch->perf.faults == f0 + 1 && strstr(ch->perf.last_err, "time limit"));
    g_now = 1000;
    CHECK(run(ch, "(defn spin (x:int) (if t (spin x) 0)) (def z (spin 1))", e) == 0);                  /* tail loops too */
    CHECK(ch->perf.faults == f0 + 2 && strstr(ch->perf.last_err, "time limit"));
    ch->deadline_us = 0;
    CHECK(run(ch, "(repeat 50 (fn () (fill (buf O) #000000)))", e) == 0 && ch->perf.faults == f0 + 2);  /* no limit set */
    dsl_set_clock(NULL);
    ch->deadline_us = 100;
    CHECK(run(ch, "(repeat 50 (fn () (fill (buf O) #000000)))", e) == 0 && ch->perf.faults == f0 + 2);  /* no clock */
    ch->deadline_us = 0;
}

/* (use name): a fake board library */
static const struct { const char *name, *src; } k_libs[] = {
    { "math",   "; helpers\n(defn dbl (x:int) (* x 2))\n(defn quad (x:int) (dbl (dbl x)))" },
    { "more",   "(use math) (defn oct (x:int) (dbl (quad x)))" },
    { "more2",  "(use math) (defn six (x:int) (+ (quad x) (dbl x)))" },
    { "both",   "(use more) (use more2) (defn all (x:int) (+ (oct x) (six x)))" },
    { "bad",    "(defn ok () 1)\n(fill (buf O) #FF0000)" },
    { "broken", "(defn fine () 1)\n\n(defn oops () (nothing-here))" },
    { "loop-a", "(use loop-b)" }, { "loop-b", "(use loop-a)" }, { "self", "(use self)" },
    { "syntax", "(defn f () (" }, { "dup", "(defn dbl (y:int) y)" },
    { "circ",   "(defn p () (q)) (defn q () (p))" },
    { "l1", "(defn f1 () 1)" }, { "l2", "(defn f2 () 1)" }, { "l3", "(defn f3 () 1)" }, { "l4", "(defn f4 () 1)" },
    { "l5", "(defn f5 () 1)" }, { "l6", "(defn f6 () 1)" }, { "l7", "(defn f7 () 1)" }, { "l8", "(defn f8 () 1)" }, { "l9", "(defn f9 () 1)" },
};
static char *fl_load(const char *name)
{
    for (size_t i = 0; i < sizeof k_libs / sizeof k_libs[0]; i++)
        if (!strcmp(k_libs[i].name, name)) return strdup(k_libs[i].src);
    return NULL;
}
static const char *fl_why(const char *name) { return strcmp(name, "twice") ? NULL : "'twice' is in two folders"; }

/* Against the US Naval Observatory's almanac (astro_cases.h). */
static void check_event(const char *place, double lat, double lon, int64_t day, double alt, bool rising, int want, int *worst)
{
    int64_t t;
    bool got = astro_sun_event(lat, lon, alt, rising, day, day + 86400, &t);
    if (want < 0) {
        if (got) { printf("  %s alt %.1f %s: almanac has none, got minute %d\n", place, alt, rising ? "rise" : "set", (int)((t - day) / 60)); g_fail++; }
        g_checks++;
        return;
    }
    if (!got) { printf("  %s alt %.1f %s: almanac %d, got none\n", place, alt, rising ? "rise" : "set", want); g_fail++; g_checks++; return; }
    int err = abs((int)(t - day) - want * 60);               /* the almanac rounds to the minute */
    if (err > *worst) *worst = err;
    if (err > 60) printf("  %s alt %.1f %s: almanac %d, got %.1f\n", place, alt, rising ? "rise" : "set", want, (t - day) / 60.0);
    CHECK(err <= 60);
}

static void t_astro(void)
{
    int worst = 0;
    double worst_lit = 0, worst_angle = 0;
    for (unsigned i = 0; i < sizeof k_sun_cases / sizeof k_sun_cases[0]; i++) {
        const char *n = k_sun_cases[i].place;
        double la = k_sun_cases[i].lat, lo = k_sun_cases[i].lon;
        int64_t d = k_sun_cases[i].day;
        check_event(n, la, lo, d, ASTRO_CIVIL, true, k_sun_cases[i].dawn, &worst);
        check_event(n, la, lo, d, ASTRO_SUNRISE, true, k_sun_cases[i].rise, &worst);
        check_event(n, la, lo, d, ASTRO_SUNRISE, false, k_sun_cases[i].set, &worst);
        check_event(n, la, lo, d, ASTRO_CIVIL, false, k_sun_cases[i].dusk, &worst);
        double e = fabs(astro_moon_lit(d + LIT_AT) * 100 - k_sun_cases[i].lit);
        if (e > worst_lit) worst_lit = e;
        CHECK(e <= 1.0);
    }
    for (unsigned i = 0; i < sizeof k_moon_cases / sizeof k_moon_cases[0]; i++) {
        double a = astro_moon_angle(k_moon_cases[i].t) - k_moon_cases[i].angle;
        if (a > 180) a -= 360;
        if (a < -180) a += 360;
        if (fabs(a) > worst_angle) worst_angle = fabs(a);
        CHECK(fabs(a) <= 0.05);                              /* the moon moves 0.05 degree in about 5.5 minutes */
    }
    printf("astro: worst sun %d s, moon lit %.2f%%, moon angle %.3f deg\n", worst, worst_lit, worst_angle);
    /* Sun altitude (env daylight): at each sunrise/dusk found above it is at that altitude; on a day with neither sunrise
     * nor sunset (polar day or night) it stays on one side all day. */
    double worst_alt = 0;
    for (unsigned i = 0; i < sizeof k_sun_cases / sizeof k_sun_cases[0]; i++) {
        double la = k_sun_cases[i].lat, lo = k_sun_cases[i].lon;
        int64_t d = k_sun_cases[i].day, ev;
        static const double alts[] = { ASTRO_SUNRISE, ASTRO_CIVIL };
        for (int j = 0; j < 4; j++)
            if (astro_sun_event(la, lo, alts[j & 1], j < 2, d, d + 86400, &ev)) {
                double e = fabs(astro_sun_altitude(la, lo, ev) - alts[j & 1]);
                if (e > worst_alt) worst_alt = e;
                CHECK(e <= 0.02);                            /* about 6 s of the sun's rise near the equator */
                double before = astro_sun_altitude(la, lo, ev - 120), after = astro_sun_altitude(la, lo, ev + 120);
                CHECK(j < 2 ? before < alts[j & 1] && after > alts[j & 1] : before > alts[j & 1] && after < alts[j & 1]);
            }
        if (k_sun_cases[i].rise < 0 && k_sun_cases[i].set < 0) {
            bool up = astro_sun_altitude(la, lo, d) > ASTRO_SUNRISE, same = true;
            for (int64_t t = d; t < d + 86400; t += 600) same &= (astro_sun_altitude(la, lo, t) > ASTRO_SUNRISE) == up;
            CHECK(same);
        }
    }
    printf("astro: sun altitude at its events within %.4f deg\n", worst_alt);
    /* The window decides: the day's sunrise is found in its day and not in the next. */
    int64_t t, day = 1782000000;                             /* 2026-06-21 00:00 UTC, Greenwich sunrise 03:43 */
    CHECK(astro_sun_event(51.48, 0, ASTRO_SUNRISE, true, day, day + 86400, &t) && t > day && t < day + 4 * 3600);
    CHECK(!astro_sun_event(51.48, 0, ASTRO_SUNRISE, true, day + 4 * 3600, day + 12 * 3600, &t));
    /* Local days: New York around the 2026 changes of clock. Sunrise jumps by the hour, less a minute or so of season. */
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1); tzset();
    astro_day_t a, b;
    astro_local_day(1772841600 + 18 * 3600, 40.71, -74.01, &a);   /* 2026-03-07 18:00 UTC */
    astro_local_day(1772928000 + 18 * 3600, 40.71, -74.01, &b);   /* 2026-03-08, the 23-hour day */
    CHECK(a.sunrise > 6 * 60 && a.sunrise < 7 * 60 && a.sunset > 17 * 60 && a.sunset < 18 * 60);
    CHECK(b.sunrise - a.sunrise >= 57 && b.sunrise - a.sunrise <= 60);
    CHECK(b.sunset - a.sunset >= 60 && b.sunset - a.sunset <= 63);
    astro_local_day(1793404800 + 18 * 3600, 40.71, -74.01, &a);    /* 2026-10-31 */
    astro_local_day(1793491200 + 18 * 3600, 40.71, -74.01, &b);   /* 2026-11-01, the 25-hour day */
    CHECK(a.sunrise - b.sunrise >= 58 && a.sunrise - b.sunrise <= 60);
    CHECK(a.dawn > 0 && a.dawn < a.sunrise && a.sunrise < a.sunset && a.sunset < a.dusk);
    /* A time zone that doesn't match the place still gives that day's events. */
    setenv("TZ", "UTC0", 1); tzset();
    astro_local_day(1782000000 + 3600, 21.31, -157.86, &a);       /* Honolulu in UTC: sets before it rises */
    CHECK(abs(a.sunrise - 950) <= 1 && abs(a.sunset - 316) <= 1);
    astro_local_day(1782000000 + 3600, 69.65, 18.96, &a);         /* midnight sun */
    CHECK(a.sunrise == -1 && a.sunset == -1 && a.dawn == -1 && a.dusk == -1);
    /* The poles and the date line don't break it. */
    CHECK(!astro_sun_event(90, 0, ASTRO_SUNRISE, true, day, day + 86400, &t));
    CHECK(!astro_sun_event(-90, 0, ASTRO_SUNRISE, false, day, day + 86400, &t));
    CHECK(astro_sun_event(0, 180, ASTRO_SUNRISE, true, day, day + 86400, &t));
    CHECK(astro_sun_event(0, -180, ASTRO_SUNRISE, true, day, day + 86400, &t));
    /* Every day of a year at high latitude: rise and set never land outside the day, and polar seasons are contiguous. */
    int changes = 0; bool had = true;
    for (int i = 0; i < 365; i++) {
        int64_t d = 1767225600 + (int64_t)i * 86400;         /* 2026-01-01 */
        bool has = astro_sun_event(69.65, 18.96, ASTRO_SUNRISE, true, d, d + 86400, &t);
        CHECK(!has || (t >= d && t < d + 86400));
        if (i && has != had) changes++;
        had = has;
    }
    CHECK(changes <= 6);                                     /* polar night ends, midnight sun starts/ends, polar night starts, plus a rise slipping past midnight */
}

static void t_use(channel_t *ch)
{
    char e[DSL_ERR_LEN];
    dsl_set_host(NULL);
    expect_err(ch, "(use math)", "programs can't use libraries here");
    static const dsl_host_t host = { .lib_load = fl_load, .lib_why = fl_why };
    dsl_set_host(&host);
    CHECK(holds(ch, "(use math)", "(eq (quad 3) 12)"));
    CHECK(holds(ch, "(def r (dbl 4)) (use math)", "(eq r 8)"));                     /* anywhere at the top level */
    CHECK(holds(ch, "(use more)", "(eq (oct 1) 8)"));                               /* through another library */
    CHECK(holds(ch, "(use more)", "(eq (quad 1) 4)"));                              /* flat: its libraries' functions too */
    CHECK(holds(ch, "(use both) (use math)", "(eq (all 1) 14)"));                   /* reached twice = loaded once */
    CHECK(holds(ch, "(use math) (defn tw (c:color i:int) c) (fill (buf Y) #FF0000) (map-px (buf Y) (buf Y) tw)", "(eq (dbl 2) 4)"));
    CHECK(holds(ch, "(use broken)", "(eq (fine) 1)"));                              /* only what is reached is compiled */
    CHECK(run(ch, "(use circ) (fill (buf O) #000000)", e) == 0);                     /* an unused circle doesn't matter */

    static const struct { const char *src, *want; } errs[] = {
        { "(fill (buf O) #000000)\n  (use nope)",          "line 2 col 8: no library program 'nope'" },
        { "(use twice)",                                   "line 1 col 6: 'twice' is in two folders" },
        { "(use)",                                         "'use' takes a library program's name" },
        { "(use math more)",                               "'use' takes a library program's name" },
        { "(use \"math\")",                                "'use' takes a library program's name" },
        { "(if t (use math))",                             "use only works at the top level" },
        { "(defn f () (use math) 1)",                      "use only works at the top level" },
        { "\n(use bad)",                                   "line 2 col 1: in bad line 2: a library for 'use' may contain only defn and use" },
        { "(use loop-a)",                                  "'loop-a' uses itself" },
        { "(use self)",                                    "line 1 col 1: in self line 1: 'self' uses itself" },
        { "(use syntax)",                                  "in syntax line 1: unclosed" },
        { "(use broken) (fill (buf O) #000000 0 (oops))",  "line 1 col 1: in broken line 3: unknown function 'nothing-here'" },
        { "(use math) (defn dbl (x:int) x)",               "'dbl' is defined twice" },
        { "(use math) (use dup)",                          "line 1 col 12: in dup line 1: 'dbl' is defined twice" },
        { "(use circ) (def x (p))",                        "line 1 col 1: in circ line 1: 'q' and 'p' call each other" },
        { "(use math) (fill (buf O) #000000 0 (dbl 1.5))", "expected int, got number" },
        { "(use l1)(use l2)(use l3)(use l4)(use l5)(use l6)(use l7)(use l8)(use l9)", "too many libraries" },
    };
    for (size_t i = 0; i < sizeof errs / sizeof errs[0]; i++) expect_err(ch, errs[i].src, errs[i].want);
    CHECK(run(ch, "(use l1)(use l2)(use l3)(use l4)(use l5)(use l6)(use l7)(use l8)", e) == 0);   /* the limit itself */
    dsl_set_host(NULL);
}

int main(void)
{
    CHECK(globals_init(&globals, 10) == 0);
    with_channel(t_fill); with_channel(t_scale_and_persistence); with_channel(t_invert); with_channel(t_nesting_globals);
    with_channel(t_slice); with_channel(t_slice_negative_step); with_channel(t_copy); with_channel(t_screen); with_channel(t_blends); with_channel(t_env); with_channel(t_fill_grad); with_channel(t_grad_map); with_channel(t_fill_random); with_channel(t_set_get); with_channel(t_rotate); with_channel(t_logic); with_channel(t_arith); with_channel(t_math_helpers); with_channel(t_env_npix); with_channel(t_env_time); with_channel(t_time_cmp); with_channel(t_seg7); with_channel(t_strings); with_channel(t_seg_text); with_channel(t_automation); with_channel(t_io); with_channel(t_inputs); with_channel(t_two_step_and_long); with_channel(t_fill_pat); with_channel(t_vars); with_channel(t_functions); with_channel(t_use); t_astro(); with_channel(t_errors); with_channel(t_swap_atomic);
    t_arena(); t_ws_parallel(); t_gamma(); t_dither(); t_wire(); t_wire_white(); t_wire_wmode(); t_fader(); t_intensity16(); t_px_scale_exhaustive(); t_blend_math(); t_stats();
    for (int i = 0; i < DSL_NUM_GLOBALS; i++) free(globals.buf[i]);
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail != 0;
}
