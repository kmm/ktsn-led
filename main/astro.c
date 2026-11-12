#define _POSIX_C_SOURCE 200809L    /* localtime_r */
#include "astro.h"
#include <math.h>

#define RAD      0.017453292519943295
#define DELTA_T  70.0                /* seconds from UT to the uniform time the orbits run on; changes ~1 s in years */

static double sind(double x) { return sin(x * RAD); }
static double cosd(double x) { return cos(x * RAD); }
static double days_j2000(double t) { return t / 86400.0 - 10957.5; }
static double wrap180(double a) { a = fmod(a, 360.0); return a > 180.0 ? a - 360.0 : a <= -180.0 ? a + 360.0 : a; }

/* The sun's apparent ecliptic longitude at time t, degrees. Omega (for nutation) on the side for the obliquity. */
static double sun_longitude(double t, double *omega)
{
    double T = days_j2000(t + DELTA_T) / 36525.0;
    double L = 280.46646 + T * (36000.76983 + T * 0.0003032);
    double M = 357.52911 + T * (35999.05029 - T * 0.0001537);
    double C = (1.914602 - T * (0.004817 + T * 0.000014)) * sind(M) + (0.019993 - T * 0.000101) * sind(2 * M) + 0.000289 * sind(3 * M);
    *omega = 125.04 - 1934.136 * T;
    return L + C - 0.00569 - 0.00478 * sind(*omega);
}

/* The sun's declination and its hour angle at longitude lon, degrees (hour angle -180..180, 0 at solar noon). */
static void sun_at(double t, double lon, double *dec, double *hour)
{
    double omega, lam = sun_longitude(t, &omega);
    double T = days_j2000(t + DELTA_T) / 36525.0;
    double eps = 23.439291 - 0.0130042 * T + 0.00256 * cosd(omega);
    double ra = atan2(cosd(eps) * sind(lam), cosd(lam)) / RAD;
    double gmst = 280.46061837 + 360.98564736629 * days_j2000(t);
    *dec = asin(sind(eps) * sind(lam)) / RAD;
    *hour = wrap180(gmst + lon - ra);
}

/* Rising or setting belonging to the solar day around `noon`. Each round corrects the time by the hour angle still
 * missing, with the declination of the current estimate. Out of reach during the rounds is clamped (the declination
 * may still bring it within reach, on the first or last day of a polar season), only the last round decides. */
static bool event_near(double lat, double lon, double altitude, bool rising, double noon, double *out)
{
    enum { ROUNDS = 5 };
    double t = noon;
    for (int i = 0; i < ROUNDS; i++) {
        double dec, hour;
        sun_at(t, lon, &dec, &hour);
        double c = (sind(altitude) - sind(lat) * sind(dec)) / (cosd(lat) * cosd(dec));
        if (fabs(c) > 1.0) {
            if (i == ROUNDS - 1) return false;
            c = c > 0 ? 1.0 : -1.0;
        }
        double target = acos(c) / RAD * (rising ? -1.0 : 1.0);
        double miss = i ? wrap180(target - hour) : target - hour;      /* the first step can be nearly half a turn */
        t += miss / 360.0 * 86400.0;
    }
    *out = t;
    return true;
}

bool astro_sun_event(double lat, double lon, double altitude, bool rising, int64_t from, int64_t to, int64_t *out)
{
    const double day = 86400.0, reach = 13 * 3600.0;   /* an event is at most half a day and the equation of time from noon */
    double noon = floor((double)from / day) * day + day / 2 - lon / 360.0 * day - day;
    for (; noon - reach < (double)to; noon += day) {
        double t;
        if (noon + reach < (double)from || !event_near(lat, lon, altitude, rising, noon, &t)) continue;
        int64_t s = (int64_t)floor(t + 0.5);
        if (s >= from && s < to) { *out = s; return true; }
    }
    return false;
}

double astro_sun_altitude(double lat, double lon, int64_t t)
{
    double dec, hour;
    sun_at((double)t, lon, &dec, &hour);
    return asin(sind(lat) * sind(dec) + cosd(lat) * cosd(dec) * cosd(hour)) / RAD;
}

static int16_t local_minutes(double lat, double lon, double alt, bool rising, int64_t from, int64_t to)
{
    int64_t t;
    if (!astro_sun_event(lat, lon, alt, rising, from, to, &t)) return -1;
    time_t x = (time_t)t;
    struct tm tm;
    localtime_r(&x, &tm);
    int m = tm.tm_hour * 60 + tm.tm_min + (tm.tm_sec >= 30);
    return (int16_t)(m > 1439 ? 1439 : m);
}

void astro_local_day(time_t now, double lat, double lon, astro_day_t *out)
{
    struct tm tm;
    localtime_r(&now, &tm);
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0; tm.tm_isdst = -1;
    int64_t from = (int64_t)mktime(&tm);
    localtime_r(&now, &tm);
    tm.tm_mday++; tm.tm_hour = tm.tm_min = tm.tm_sec = 0; tm.tm_isdst = -1;
    int64_t to = (int64_t)mktime(&tm);                 /* 23 or 25 hours later on a changeover day */
    out->dawn    = local_minutes(lat, lon, ASTRO_CIVIL, true, from, to);
    out->sunrise = local_minutes(lat, lon, ASTRO_SUNRISE, true, from, to);
    out->sunset  = local_minutes(lat, lon, ASTRO_SUNRISE, false, from, to);
    out->dusk    = local_minutes(lat, lon, ASTRO_CIVIL, false, from, to);
}

/* Longitude terms of the moon (Meeus, chapter 47), largest first: multiples of D, M, M', F and 1e-6 degrees. */
static const struct { int8_t d, m, mp, f; int32_t l; } k_moon[] = {
    { 0, 0, 1, 0, 6288774 }, { 2, 0,-1, 0, 1274027 }, { 2, 0, 0, 0,  658314 }, { 0, 0, 2, 0,  213618 },
    { 0, 1, 0, 0, -185116 }, { 0, 0, 0, 2, -114332 }, { 2, 0,-2, 0,   58793 }, { 2,-1,-1, 0,   57066 },
    { 2, 0, 1, 0,   53322 }, { 2,-1, 0, 0,   45758 }, { 0, 1,-1, 0,  -40923 }, { 1, 0, 0, 0,  -34720 },
    { 0, 1, 1, 0,  -30383 }, { 2, 0, 0,-2,   15327 }, { 0, 0, 1, 2,  -12528 }, { 0, 0, 1,-2,   10980 },
    { 4, 0,-1, 0,   10675 }, { 0, 0, 3, 0,   10034 }, { 4, 0,-2, 0,    8548 }, { 2, 1,-1, 0,   -7888 },
    { 2, 1, 0, 0,   -6766 }, { 1, 0,-1, 0,   -5163 }, { 1, 1, 0, 0,    4987 }, { 2,-1, 1, 0,    4036 },
};

/* The moon's longitude minus the sun's (0..360) and its latitude, degrees. */
static double moon_at(int64_t t, double *lat)
{
    double T = days_j2000((double)t + DELTA_T) / 36525.0;
    double D = 297.8501921 + 445267.1114034 * T, M = 357.5291092 + 35999.0502909 * T;
    double Mp = 134.9633964 + 477198.8675055 * T, F = 93.2720950 + 483202.0175233 * T;
    double lon = 218.3164477 + 481267.88123421 * T, sum = 0, omega;
    for (unsigned i = 0; i < sizeof k_moon / sizeof k_moon[0]; i++)
        sum += k_moon[i].l * sind(k_moon[i].d * D + k_moon[i].m * M + k_moon[i].mp * Mp + k_moon[i].f * F);
    *lat = 5.128122 * sind(F) + 0.280602 * sind(Mp + F) + 0.277693 * sind(Mp - F) + 0.173237 * sind(2 * D - F);
    double a = fmod(lon + sum * 1e-6 - sun_longitude((double)t, &omega), 360.0);
    return a < 0 ? a + 360.0 : a;
}

double astro_moon_angle(int64_t t) { double lat; return moon_at(t, &lat); }

double astro_moon_lit(int64_t t)
{
    double lat, a = moon_at(t, &lat);
    return (1.0 - cosd(lat) * cosd(a)) / 2.0;
}
