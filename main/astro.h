#pragma once
/* Sun and moon from formulas (NOAA solar equations, Meeus's Astronomical Algorithms): no tables, no expiry. Portable:
 * also built for the host tests and the IDE's WebAssembly core, so the preview matches a board. Times are unix seconds. */
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define ASTRO_SUNRISE (-0.8333)     /* altitude of the sun's centre at sunrise/sunset: refraction plus its radius */
#define ASTRO_CIVIL   (-6.0)        /* civil dawn and dusk */

/* First time in [from, to) the sun passes `altitude` degrees, rising or setting, seen from lat/lon (degrees, north and
 * east positive). False if it doesn't happen in that window (polar day or night, or it falls just outside). */
bool   astro_sun_event(double lat, double lon, double altitude, bool rising, int64_t from, int64_t to, int64_t *out);

/* The altitude of the sun's centre above the horizon at time t, degrees (no refraction: compare with ASTRO_SUNRISE). */
double astro_sun_altitude(double lat, double lon, int64_t t);

/* The local calendar day of `now` (the process's time zone): minutes after local midnight, by the clock (so they compare
 * with the hour and minute also on a daylight-saving changeover day), -1 = doesn't happen that day. */
typedef struct { int16_t dawn, sunrise, sunset, dusk; } astro_day_t;
void   astro_local_day(time_t now, double lat, double lon, astro_day_t *out);

double astro_moon_angle(int64_t t);     /* 0 <= moon's longitude - sun's < 360: 0 new, 90 first quarter, 180 full, 270 last */
double astro_moon_lit(int64_t t);       /* fraction of the disc lit, 0..1 */
