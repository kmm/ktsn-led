#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "dither.h"
#include "pixel.h"

/* LED output, one of two drivers (menuconfig KTSN LED -> LED output driver):
 *   RMT    (led_strip): one RMT channel per string; led_out_write sends that string right away.
 *   PARLIO (C6 etc.):   all strings share one DMA transfer, one data line each; led_out_write only encodes,
 *                       led_out_flush sends every string at once. No refill interrupts, so WiFi can't disturb it.
 * Call order: led_out_init per channel, led_out_start once, then per frame led_out_write per channel + led_out_flush. */
/* wire: the channel's wire format string (wire.h); an invalid one falls back to WIRE_DEFAULT. */
esp_err_t led_out_init(int idx, int gpio, uint16_t npix, const char *wire);
esp_err_t led_out_start(void);
/* Maps each pixel with `m` (gamma, intensity, dither: dither.h), then sends the bytes its wire format names. */
void      led_out_write(int idx, const pixel_t *px, uint16_t n, const out_map_t *m);
void      led_out_flush(void);
/* RMT: start every string's send and wait for them together in led_out_flush (on), or send each one before returning
 * from led_out_write (off, the default). PARLIO always sends all strings at once; this is ignored there. */
void      led_out_set_parallel(bool on);
/* Estimated time to send channel idx's string (its bytes, incl. the reset gap; 0 if it isn't running), and whether
 * strings go out at the same time. */
uint32_t  led_out_send_us(int idx);
/* The wire format channel idx runs with (from boot), or "" if it isn't running. */
const char *led_out_wire(int idx);
bool      led_out_concurrent(void);
const char *led_out_driver(void);   /* "rmt" or "parlio" */
