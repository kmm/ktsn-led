#pragma once
#include <stdbool.h>
#include <time.h>
#include "cmd.h"

/* WiFi station + SNTP, configured from g_set. All of these are safe to call repeatedly. */
void net_wifi_apply(void);     /* connect with g_set.ssid/pass; empty ssid stops WiFi */
void net_ntp_apply(void);      /* restart SNTP with g_set.ntp (if connected) */
void net_hostname_apply(void); /* g_set.hostname -> mDNS now, DHCP from the next connect */
void net_print_status(cmd_out_t *o);   /* wifi + ntp state for the console */

typedef struct {
    bool   connected, ntp_running, ntp_synced;
    char   ip[16];
    int    rssi;               /* dBm, 0 if not connected */
    time_t last_sync;
} net_status_t;
void net_get_status(net_status_t *st);
