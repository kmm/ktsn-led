#include "audio.h"
#include <stdint.h>
#include <string.h>

/* round(32767 * cos(2 pi k / 256)), k = 0..64: a quarter wave; the rest by symmetry. */
static const int16_t k_cos[65] = {
    32767, 32757, 32728, 32678, 32609, 32521, 32412, 32285, 32137, 31971, 31785, 31580, 31356,
    31113, 30852, 30571, 30273, 29956, 29621, 29268, 28898, 28510, 28105, 27683, 27245, 26790,
    26319, 25832, 25329, 24811, 24279, 23731, 23170, 22594, 22005, 21403, 20787, 20159, 19519,
    18868, 18204, 17530, 16846, 16151, 15446, 14732, 14010, 13279, 12539, 11793, 11039, 10278,
    9512, 8739, 7962, 7179, 6393, 5602, 4808, 4011, 3212, 2410, 1608, 804, 0
};

static int32_t cos256(int k)                     /* cos(2 pi k / 256), Q15 */
{
    k &= 255;
    if (k <= 64)  return k_cos[k];
    if (k <= 128) return -k_cos[128 - k];
    if (k <= 192) return -k_cos[k - 128];
    return k_cos[256 - k];
}
static int32_t sin256(int k) { return cos256(k - 64); }

/* Bins are 31.25 Hz wide: 31-62, 62-125, 125-219, 219-375, 375-625, 625-1031, 1031-1719, 1719-4000 Hz. */
const uint8_t audio_band_edges[AUDIO_BANDS + 1] = { 1, 2, 4, 7, 12, 20, 33, 55, 128 };
#define BASS_END 4                               /* bins 1..3 (31..125 Hz) make the beat signal */

/* Automatic gain, in log2 power Q8 (256 = 3 dB). The bands share one gain (from the loudest band), so they show
 * how the sound is spread; the bands are about an octave wide, so pink noise (and most music) gives them similar
 * values. The level has its own. */
#define PK_DECAY   8                             /* the peak falls ~3 dB a second */
#define FL_RISE    4                             /* the floor rises ~1.5 dB a second */
#define BAND_SPAN  2048                          /* bands: 0..1 covers at least 24 dB */
#define LEVEL_SPAN 1280                          /* level: at least 15 dB */
#define MAX_SPAN   3072                          /* both: at most 36 dB */
#define RELEASE    (AUDIO_ONE / 6)               /* outputs fall at most this much per block (~200 ms to 0) */
#define BEAT_RISE  256                           /* bass at least 3 dB above its median of the last second, and */
#define BEAT_SPREAD 4                            /* 4 x (median - lower quartile) above it (~2.7 sd for normal noise) */
#define BEAT_GAP   250                           /* and at least this many ms after the last beat */
#define BEAT_QUIET 2048                          /* no beats from bass more than 24 dB below the loudest band */

void audio_init(audio_t *a) { memset(a, 0, sizeof *a); }

int32_t audio_log2q8(uint64_t x)
{
    if (x <= 1) return 0;
    int msb = 63 - __builtin_clzll(x);
    uint32_t frac = msb >= 8 ? (uint32_t)(x >> (msb - 8)) & 0xFF : (uint32_t)(x << (8 - msb)) & 0xFF;
    /* log2(1 + f) ~ f + 0.343 f (1 - f): within 0.009 instead of 0.086 for the straight line */
    uint32_t corr = (frac * (256 - frac) * 88u) >> 16;
    return msb * 256 + (int32_t)frac + (int32_t)corr;
}

void audio_fft256(int16_t *re, int16_t *im)
{
    for (int i = 0; i < AUDIO_N; i++) {          /* bit-reversed order */
        int j = 0;
        for (int b = 0; b < 8; b++) j |= ((i >> b) & 1) << (7 - b);
        if (j > i) {
            int16_t t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    /* Radix 2, halved at every stage: a complex value's magnitude never grows past the largest input magnitude, so
     * inputs within +-16384 (magnitude <= 23170) can't overflow. */
    for (int len = 2; len <= AUDIO_N; len <<= 1) {
        int half = len / 2, step = AUDIO_N / len;
        for (int k = 0; k < half; k++) {
            int32_t wr = cos256(k * step), wi = -sin256(k * step);
            for (int i = k; i < AUDIO_N; i += len) {
                int b = i + half;
                int32_t tr = (re[b] * wr - im[b] * wi) >> 15;
                int32_t ti = (re[b] * wi + im[b] * wr) >> 15;
                int32_t ar = re[i], ai = im[i];
                re[b] = (int16_t)((ar - tr) >> 1); im[b] = (int16_t)((ai - ti) >> 1);
                re[i] = (int16_t)((ar + tr) >> 1); im[i] = (int16_t)((ai + ti) >> 1);
            }
        }
    }
}

/* Track the gain g (0 = bands, 1 = level) from L (log2 power Q8): a peak that falls slowly, a floor that rises
 * slowly; then map values into 0..AUDIO_ONE between them. */
static void gain_update(audio_t *a, int g, int32_t L, int32_t min_span)
{
    if (!a->started) { a->pk[g] = L; a->fl[g] = L - min_span; }
    if (L > a->pk[g]) a->pk[g] = L; else a->pk[g] -= PK_DECAY;
    if (L < a->fl[g]) a->fl[g] = L; else a->fl[g] += FL_RISE;
}

static uint32_t gain_map(const audio_t *a, int g, int32_t L, int32_t min_span)
{
    int32_t pk = a->pk[g], bot = a->fl[g];
    if (bot < pk - MAX_SPAN) bot = pk - MAX_SPAN;
    if (bot > pk - min_span) bot = pk - min_span;
    if (L <= bot) return 0;
    if (L >= pk) return AUDIO_ONE;
    return (uint32_t)((uint64_t)(L - bot) * AUDIO_ONE / (uint32_t)(pk - bot));
}

static uint32_t release(uint32_t old, uint32_t now) { return now >= old ? now : (old - now > RELEASE ? old - RELEASE : now); }

void audio_block(audio_t *a, const uint16_t *s, uint32_t now_ms)
{
    int16_t *re = a->re, *im = a->im;
    int32_t sum = 0;
    for (int n = 0; n < AUDIO_N; n++) sum += s[n] > 4095 ? 4095 : s[n];
    int32_t mean = (sum + AUDIO_N / 2) / AUDIO_N;
    int32_t swing = 0;
    for (int n = 0; n < AUDIO_N; n++) {
        int32_t d = (s[n] > 4095 ? 4095 : s[n]) - mean;
        if (d < 0) d = -d;
        if (d > swing) swing = d;
    }
    a->swing = (uint16_t)swing;
    a->beat = false;
    if (swing < AUDIO_GATE) {                    /* silence: let everything fall, learn nothing */
        for (int b = 0; b < AUDIO_BANDS; b++) a->band[b] = release(a->band[b], 0);
        a->level = release(a->level, 0);
        return;
    }
    int sh = 0;                                  /* use the FFT's range whatever the volume (undone in the logs) */
    while (sh < 14 && (swing << (sh + 1)) <= 16383) sh++;
    for (int n = 0; n < AUDIO_N; n++) {
        int32_t d = ((s[n] > 4095 ? 4095 : s[n]) - mean) * (1 << sh);
        int32_t w = (32768 - cos256(n)) >> 1;    /* Hann, Q15 */
        re[n] = (int16_t)((d * w) >> 15);
        im[n] = 0;
    }
    audio_fft256(re, im);
    uint64_t pw[AUDIO_BANDS], total = 0, bass = 0;
    for (int b = 0; b < AUDIO_BANDS; b++) {
        uint64_t p = 0;
        for (int k = audio_band_edges[b]; k < audio_band_edges[b + 1]; k++) {
            uint64_t e = (uint64_t)((int32_t)re[k] * re[k]) + (uint64_t)((int32_t)im[k] * im[k]);
            p += e;
            if (k < BASS_END) bass += e;
        }
        pw[b] = p;
        total += p;
    }
    int32_t undo = 512 * sh;                     /* power grew by 4^sh */
    int32_t L[AUDIO_BANDS], top = INT32_MIN;
    for (int b = 0; b < AUDIO_BANDS; b++) { L[b] = audio_log2q8(pw[b]) - undo; if (L[b] > top) top = L[b]; }
    int32_t lt = audio_log2q8(total) - undo;
    gain_update(a, 0, top, BAND_SPAN);
    gain_update(a, 1, lt, LEVEL_SPAN);
    for (int b = 0; b < AUDIO_BANDS; b++) a->band[b] = release(a->band[b], gain_map(a, 0, L[b], BAND_SPAN));
    a->level = release(a->level, gain_map(a, 1, lt, LEVEL_SPAN));
    a->started = true;

    int32_t lb = audio_log2q8(bass) - undo;
    if (a->hist_n >= AUDIO_HIST && lb > top - BEAT_QUIET) {
        /* The median and lower quartile of the last second: unlike a mean and deviation, the beats themselves
         * barely move them, so a steady beat doesn't raise its own threshold. */
        int32_t h[AUDIO_HIST];
        for (int k = 0; k < AUDIO_HIST; k++) {
            int32_t v = a->hist[k], j = k;
            while (j > 0 && h[j - 1] > v) { h[j] = h[j - 1]; j--; }
            h[j] = v;
        }
        int32_t med = h[AUDIO_HIST / 2], rise = (med - h[AUDIO_HIST / 4]) * BEAT_SPREAD;
        if (rise < BEAT_RISE) rise = BEAT_RISE;
        if (lb > med + rise && (!a->beats || (uint32_t)(now_ms - a->beat_at) >= BEAT_GAP)) {
            a->beats++;
            a->beat_at = now_ms;
            a->beat = true;
        }
    }
    a->hist[a->hist_at] = lb;
    a->hist_at = (uint8_t)((a->hist_at + 1) % AUDIO_HIST);
    if (a->hist_n < AUDIO_HIST) a->hist_n++;
}
