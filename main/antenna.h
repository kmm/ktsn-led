#pragma once
#include "sdkconfig.h"

/* Seeed XIAO ESP32-C6 RF switch (CONFIG_KTSN_XIAO_C6_ANTENNA): GPIO3 low powers the switch, GPIO14 picks the antenna
 * (low = the built-in ceramic one, high = the U.FL connector). The choice is g_set.antenna_ext. */
#if CONFIG_KTSN_XIAO_C6_ANTENNA
#define ANTENNA_EN_GPIO  3
#define ANTENNA_SEL_GPIO 14
void antenna_apply(void);              /* drive the pins from g_set.antenna_ext; at boot before WiFi, and on change */
#endif
