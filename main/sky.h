#pragma once
#include <stdint.h>

#define SKY_CLOCK_SET_AFTER 1700000000      /* unix time before 2023: the clock hasn't been set (NTP or env time) */

/* Keeps the sun and moon values of g_env current from g_set's location and the clock. Called from a low-priority task
 * (and once at boot, before anything runs); recomputes when the minute or the location changes, `daylight` each second. */
void     sky_tick(void);
uint32_t sky_last_us(void);     /* how long the last computation took */
