#pragma once
#include <stdint.h>

/* Per-channel gamma correction at output: out = round(255 * (in/255)^gamma), as a 256-entry table per channel.
 * Portable and integer-only (no libm), so the boards and the IDE preview (WebAssembly) build identical tables.
 * Gamma is kept in hundredths: 100 = 1.00 = off (identity), 220 = 2.2. */
#define GAMMA_OFF  100
#define GAMMA_MIN  100
#define GAMMA_MAX  300

void gamma_lut(uint8_t lut[256], unsigned g100);
/* The same curve in 8.8 fixed point (round(65280 * (in/255)^gamma)), for dithering: off = in << 8. */
void gamma_lut16(uint16_t lut[256], unsigned g100);
/* "2.2", "2.25", "1", "off" -> hundredths in GAMMA_MIN..GAMMA_MAX. NULL if fine, else a short reason. */
const char *gamma_parse(const char *s, unsigned *g100);
/* "2.20", or "off" for GAMMA_OFF; buf needs 8 bytes. */
void gamma_format(char *buf, unsigned g100);
