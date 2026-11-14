#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Audio analysis for beat-reactive programs (portable: host tests, no ESP-IDF). Integer only.
 * Blocks of AUDIO_N samples at AUDIO_RATE (32 ms each, back to back) go through a Hann window and a 256-point
 * fixed-point FFT; the power spectrum is summed into AUDIO_BANDS log-spaced bands, which are measured in a log scale
 * and normalised by an automatic gain shared by the bands (a peak that falls slowly, a floor that rises slowly), so
 * they move between 0 and 1 whatever the volume and show how the sound is spread. A beat is a jump of the bass (31..125 Hz) well above its average over
 * the last second. Quality is deliberately modest: enough for lights that follow music. */
#define AUDIO_N       256
#define AUDIO_RATE    8000
#define AUDIO_HOP_MS  32                 /* AUDIO_N / AUDIO_RATE */
#define AUDIO_BANDS   8
#define AUDIO_ONE     65536              /* band / level scale: 1.0 */
#define AUDIO_HIST    32                 /* bass history for beats: ~1 s */
#define AUDIO_GATE    6                  /* a block whose samples all stay within this of their mean is silence */
#define AUDIO_GATE_MAX 2000              /* the noise gate (RMS in ADC counts) at most */

typedef struct {
    /* results of the latest block */
    uint32_t band[AUDIO_BANDS];          /* 0..AUDIO_ONE, low to high */
    uint32_t level;                      /* overall, 0..AUDIO_ONE */
    uint32_t beats;                      /* beats so far */
    uint32_t beat_at;                    /* now_ms of the latest beat (valid when beats > 0) */
    uint16_t swing;                      /* largest |sample - mean| in the block, 0..4095 (to set a mic's gain) */
    uint16_t rms;                        /* RMS of (sample - mean) in the block, ADC counts (compared with the gate) */
    bool     beat;                       /* the latest block was a beat */
    /* set by the caller (audio_init clears it): the noise gate, RMS in ADC counts, 0 = off. A block below it is
     * silence (outputs fall, nothing is learned), and the automatic gain treats it as the bottom of the range, so
     * room noise stays dark however long it is quiet. */
    uint16_t gate;
    /* state */
    int32_t  pk[2], fl[2];               /* log2 power, Q8: peak and floor of the gains (0 bands, 1 level) */
    int32_t  hist[AUDIO_HIST];           /* bass log2 power, Q8 */
    uint8_t  hist_at, hist_n;
    bool     started;
    int16_t  re[AUDIO_N], im[AUDIO_N];   /* FFT work space (each analysis has its own: the bench runs beside the input) */
} audio_t;

void audio_init(audio_t *a);
/* One block: AUDIO_N raw samples 0..4095 (12-bit ADC), and the time in ms (for beat spacing). */
void audio_block(audio_t *a, const uint16_t *s, uint32_t now_ms);

/* The gate for a quiet room, from the RMS of n blocks of its noise (sorted in place): the 90th percentile (a click
 * or two while learning doesn't count) plus ~3.5 dB. At least 1, at most AUDIO_GATE_MAX; 0 if n is 0. */
uint16_t audio_gate_learn(uint16_t *rms, int n);

/* Building blocks, exposed for the tests. */
void     audio_fft256(int16_t *re, int16_t *im);      /* in place, each stage halved: result = DFT / 256 */
int32_t  audio_log2q8(uint64_t x);                    /* log2(x) in Q8 (0 for x <= 1), error < 0.09 */
extern const uint8_t audio_band_edges[AUDIO_BANDS + 1];   /* FFT bins: band b = bins [e[b], e[b+1]) */
