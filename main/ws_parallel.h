#pragma once
#include <stddef.h>
#include <stdint.h>

/* Parallel WS281x encoder for a parallel output peripheral (PARLIO): lane j of every sample drives strip j.
 * Each data bit is WS_SAMPLES_PER_BIT samples of WS_SAMPLE_NS: 0 -> 10000 (300 ns high, 1200 low),
 * 1 -> 11100 (900 high, 600 low). The high times match led_strip's WS2812 RMT timing exactly.
 * Samples are `width` bits each, packed LSB first (PARLIO_BIT_PACK_ORDER_LSB): sample s, lane j is bit s*width+j.
 * Portable C (no ESP-IDF), tested on the host. */

#define WS_SAMPLE_HZ        3333333u     /* 240 MHz / 72: 300 ns per sample */
#define WS_SAMPLES_PER_BIT  5u

/* Buffer bytes for strips of up to max_pixels (3 bytes each) on `width` lanes. Always a whole number of bytes. */
static inline size_t ws_par_bytes(size_t max_pixels, unsigned width)
{
    return max_pixels * 24u * WS_SAMPLES_PER_BIT * width / 8u;
}

/* Bytes to transmit per frame: ws_par_bytes rounded up to whole 32-bit words. PARLIO on the C6 can't end a transfer
 * mid-word (IDF warns; on hardware a 750-byte frame strobed red/green, likely the partial word leaking into the next
 * frame); the padding is zero
 * samples, i.e. extra low time after the data, which WS281x treat as part of the reset gap. */
static inline size_t ws_par_frame_bytes(size_t max_pixels, unsigned width)
{
    return (ws_par_bytes(max_pixels, width) + 3u) & ~(size_t)3u;
}

/* Encode `n` wire-order bytes for `lane`, starting at byte index `at` of that lane's stream.
 * Only 1 samples are written: the buffer must be zeroed before a frame is encoded. */
void ws_par_encode(uint8_t *buf, unsigned width, unsigned lane, size_t at, const uint8_t *bytes, size_t n);
