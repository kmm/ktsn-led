#include "gamma.h"
#include <stdio.h>
#include <string.h>

/* 2^(2^-j) in Q30, j = 1..16 */
static const uint32_t k_exp2_frac[16] = {
    1518500250u, 1276901417u, 1170923762u, 1121280436u, 1097253708u, 1085434106u, 1079572136u, 1076653033u,
    1075196443u, 1074468888u, 1074105294u, 1073923544u, 1073832680u, 1073787251u, 1073764537u, 1073753181u,
};

#define LOG_FRAC 28                                     /* fraction bits of the log domain */

/* log2(x) in Q28 for x in 1..255: integer part from the top bit, fraction by repeated squaring of the mantissa. */
static int64_t log2_fix(uint32_t x)
{
    int n = 0;
    while ((x >> n) > 1) n++;
    uint64_t m = ((uint64_t)x << 30) >> n;               /* mantissa, Q30 in [1, 2) */
    int64_t r = (int64_t)n << LOG_FRAC;
    for (int b = LOG_FRAC - 1; b >= 0; b--) {
        m = (m * m) >> 30;
        if (m >= (2ull << 30)) { m >>= 1; r |= (int64_t)1 << b; }
    }
    return r;
}

/* round(mult * (i/255)^gamma) for i in 1..255, gamma in hundredths (not off). */
static uint32_t gamma_val(uint32_t i, unsigned g100, uint32_t mult)
{
    const int64_t one = (int64_t)1 << LOG_FRAC;
    /* y = gamma * log2(i/255) <= 0; 2^y = 2^frac >> -q with y = q + frac, 0 <= frac < 1 */
    int64_t y = (log2_fix(i) - log2_fix(255)) * (int64_t)g100 / 100;
    int64_t q = -((-y + one - 1) >> LOG_FRAC);
    uint64_t f = (uint64_t)(y - q * one);                 /* Q28 */
    uint64_t r = 1ull << 30;                              /* Q30 */
    for (int j = 0; j < 16; j++)                          /* top 16 fraction bits from the table */
        if (f & (1ull << (LOG_FRAC - 1 - j))) r = (r * k_exp2_frac[j]) >> 30;
    uint64_t rest = f & ((1ull << (LOG_FRAC - 16)) - 1);  /* the rest: 2^x ~ 1 + x ln2 (x < 2^-16) */
    r += (r * ((rest * 744261118ull) >> 30)) >> LOG_FRAC; /* ln2 in Q30 */
    unsigned s = 30u + (unsigned)(-q);
    return s >= 62 ? 0 : (uint32_t)(((uint64_t)mult * r + (1ull << (s - 1))) >> s);
}

void gamma_lut(uint8_t lut[256], unsigned g100)
{
    if (g100 < GAMMA_MIN || g100 > GAMMA_MAX) g100 = GAMMA_OFF;
    lut[0] = 0;
    for (uint32_t i = 1; i < 256; i++) lut[i] = (uint8_t)(g100 == GAMMA_OFF ? i : gamma_val(i, g100, 255));
}

void gamma_lut16(uint16_t lut[256], unsigned g100)
{
    if (g100 < GAMMA_MIN || g100 > GAMMA_MAX) g100 = GAMMA_OFF;
    lut[0] = 0;
    for (uint32_t i = 1; i < 256; i++) lut[i] = (uint16_t)(g100 == GAMMA_OFF ? i << 8 : gamma_val(i, g100, 255u << 8));
}

const char *gamma_parse(const char *s, unsigned *g100)
{
    const char *range = "gamma must be 1.0..3.0 (up to 2 decimals) or off";
    if (!strcmp(s, "off")) { *g100 = GAMMA_OFF; return NULL; }
    unsigned v = 0, frac = 0, digits = 0;
    const char *p = s;
    if (*p < '0' || *p > '9') return range;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (unsigned)(*p++ - '0'); if (v > 9) return range; }
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') { if (++digits > 2) return range; frac = frac * 10 + (unsigned)(*p++ - '0'); }
        if (digits == 1) frac *= 10;
    }
    if (*p) return range;
    v = v * 100 + frac;
    if (v < GAMMA_MIN || v > GAMMA_MAX) return range;
    *g100 = v;
    return NULL;
}

void gamma_format(char *buf, unsigned g100)
{
    if (g100 == GAMMA_OFF) strcpy(buf, "off");
    else snprintf(buf, 8, "%u.%02u", (g100 / 100) % 10, g100 % 100);
}
