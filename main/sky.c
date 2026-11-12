#include "sky.h"
#include <math.h>
#include <time.h>
#include "astro.h"
#include "dsl.h"
#include "esp_timer.h"
#include "app.h"
#include "settings.h"

static uint32_t s_last_us;

void sky_tick(void)
{
    static int64_t last_minute = -1;
    static int32_t last_lat, last_lon;
    static bool last_has;
    static int64_t last_second = -1;
    time_t now = time(NULL);
    int64_t minute = (int64_t)now / 60;
    bool has = g_set.has_loc;
    int32_t lat = g_set.lat, lon = g_set.lon;
    bool clock_set = (int64_t)now > SKY_CLOCK_SET_AFTER;
    if ((int64_t)now != last_second || has != last_has || lat != last_lat || lon != last_lon) {
        last_second = (int64_t)now;      /* cheap: one sun position */
        g_env.daylight = (int8_t)(clock_set && has ? astro_sun_altitude(lat / 1e5, lon / 1e5, now) > ASTRO_SUNRISE : -1);
    }
    if (minute == last_minute && has == last_has && lat == last_lat && lon == last_lon) return;
    last_minute = minute; last_has = has; last_lat = lat; last_lon = lon;

    int64_t t0 = esp_timer_get_time();
    g_env.sunrise = g_env.sunset = g_env.dawn = g_env.dusk = -1;
    g_env.moon = g_env.moonlit = -1;
    if (clock_set) {
        g_env.moon = (int16_t)((int)lround(astro_moon_angle(now)) % 360);
        g_env.moonlit = (int16_t)lround(astro_moon_lit(now) * 100);
        if (has) {
            astro_day_t d;
            astro_local_day(now, lat / 1e5, lon / 1e5, &d);
            g_env.dawn = d.dawn; g_env.sunrise = d.sunrise; g_env.sunset = d.sunset; g_env.dusk = d.dusk;
        }
    }
    s_last_us = (uint32_t)(esp_timer_get_time() - t0);
}

uint32_t sky_last_us(void) { return s_last_us; }
