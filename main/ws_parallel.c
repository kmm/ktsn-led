#include "ws_parallel.h"

static inline void set_sample(uint8_t *buf, unsigned width, unsigned lane, size_t s)
{
    size_t bit = s * width + lane;
    buf[bit >> 3] |= (uint8_t)(1u << (bit & 7));
}

void ws_par_encode(uint8_t *buf, unsigned width, unsigned lane, size_t at, const uint8_t *bytes, size_t n)
{
    size_t s = at * 8u * WS_SAMPLES_PER_BIT;
    for (size_t i = 0; i < n; i++)
        for (int b = 7; b >= 0; b--, s += WS_SAMPLES_PER_BIT) {          /* MSB first, as WS281x expect */
            set_sample(buf, width, lane, s);                             /* every bit starts high */
            if ((bytes[i] >> b) & 1u) { set_sample(buf, width, lane, s + 1); set_sample(buf, width, lane, s + 2); }
        }
}
