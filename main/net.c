/* WiFi station mode and SNTP time sync. Credentials come from g_set (we persist them ourselves, so the WiFi driver
 * is told to keep its config in RAM only). Reconnects forever while an SSID is set: quickly at first, then every 10 s. */
#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "sdkconfig.h"
#include "settings.h"
#if CONFIG_KTSN_NET_CLI
#include "netcli.h"
#endif
#if CONFIG_KTSN_HTTP_API
#include "http_api.h"
#endif
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#if CONFIG_KTSN_MDNS
#include "mdns.h"
#endif

static const char *TAG = "net";

static bool s_inited, s_started, s_want, s_have_ip, s_sntp_on, s_synced, s_mdns_on;
static esp_netif_t *s_netif;
#if CONFIG_KTSN_MDNS
static void net_mdns_start(void);
#endif
static esp_timer_handle_t s_retry;
static int s_retries;
static uint8_t s_last_reason;
static char s_ip[16];
static time_t s_last_sync;

static void on_sync(struct timeval *tv)
{
    s_synced = true;
    s_last_sync = tv->tv_sec;
    struct tm tm;
    localtime_r(&tv->tv_sec, &tm);
    ESP_LOGI(TAG, "time synced: %04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static void sntp_restart(void)
{
    if (s_sntp_on) { esp_netif_sntp_deinit(); s_sntp_on = false; }
    if (!s_have_ip || !g_set.ntp[0]) return;
    esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG(g_set.ntp);
    c.sync_cb = on_sync;
    if (esp_netif_sntp_init(&c) == ESP_OK) s_sntp_on = true;
    else ESP_LOGW(TAG, "could not start SNTP");
}

static void retry_cb(void *arg)
{
    (void)arg;
    if (s_want) esp_wifi_connect();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_want) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        if (s_have_ip) ESP_LOGW(TAG, "disconnected (reason %u)", d->reason);
        s_have_ip = false;
        s_last_reason = d->reason;
        if (s_want) {
            s_retries++;
            esp_timer_stop(s_retry);
            esp_timer_start_once(s_retry, s_retries < 5 ? 1000000 : 10000000);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&e->ip_info.ip));
        s_have_ip = true;
        s_retries = 0;
        ESP_LOGI(TAG, "connected to '%s', ip %s", g_set.ssid, s_ip);
        sntp_restart();
    }
}

static void init_once(void)
{
    if (s_inited) return;
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t r = esp_event_loop_create_default();
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(r);
    s_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_netif, g_set.hostname);     /* before DHCP starts, so the router sees it */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    const esp_timer_create_args_t ta = { .callback = retry_cb, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&ta, &s_retry));
    esp_log_level_set("wifi", ESP_LOG_WARN);             /* keep the driver's chatter out of the console */
    esp_log_level_set("wifi_init", ESP_LOG_WARN);
    esp_log_level_set("phy_init", ESP_LOG_WARN);
    esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);
    s_inited = true;
#if CONFIG_KTSN_MDNS
    net_mdns_start();
#endif
#if CONFIG_KTSN_NET_CLI
    netcli_start();                                       /* listens on all interfaces; works once we have an IP */
#endif
#if CONFIG_KTSN_HTTP_API
    http_api_start();
#endif
}

void net_wifi_apply(void)
{
    if (!g_set.ssid[0]) {
        if (s_started) {
            s_want = false;
            esp_timer_stop(s_retry);
            if (s_sntp_on) { esp_netif_sntp_deinit(); s_sntp_on = false; }
            esp_wifi_disconnect();
            esp_wifi_stop();
            s_started = s_have_ip = false;
            ESP_LOGI(TAG, "WiFi off");
        }
        return;
    }
    init_once();
    wifi_config_t wc = { 0 };
    memcpy(wc.sta.ssid, g_set.ssid, strnlen(g_set.ssid, sizeof wc.sta.ssid));
    memcpy(wc.sta.password, g_set.pass, strnlen(g_set.pass, sizeof wc.sta.password));
    wc.sta.threshold.authmode = g_set.pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    s_want = true;
    s_retries = 0;
    s_have_ip = false;
    if (s_started) esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    if (!s_started) {
        ESP_ERROR_CHECK(esp_wifi_start());              /* STA_START event connects */
        s_started = true;
#if CONFIG_KTSN_WIFI_PS_NONE
        esp_wifi_set_ps(WIFI_PS_NONE);
#endif
    }
    else esp_wifi_connect();
    ESP_LOGI(TAG, "connecting to '%s'", g_set.ssid);
}

void net_ntp_apply(void) { sntp_restart(); }

#if CONFIG_KTSN_MDNS
/* <hostname>.local, plus the services this build has, for browsers like `dns-sd -B _http._tcp`. */
static void net_mdns_start(void)
{
    if (mdns_init() != ESP_OK) { ESP_LOGW(TAG, "mDNS failed to start"); return; }
    s_mdns_on = true;
    mdns_hostname_set(g_set.hostname);
    mdns_instance_name_set("KTSN LED controller");
#if CONFIG_KTSN_HTTP_API
    mdns_txt_item_t txt[] = { { "path", "/api/status" }, { "auth", "bearer" } };
    mdns_service_add(NULL, "_http", "_tcp", CONFIG_KTSN_HTTP_PORT, txt, sizeof txt / sizeof txt[0]);
#endif
#if CONFIG_KTSN_NET_CLI
    mdns_service_add(NULL, "_telnet", "_tcp", CONFIG_KTSN_NET_CLI_PORT, NULL, 0);
#endif
}
#endif

void net_hostname_apply(void)
{
    if (s_netif) esp_netif_set_hostname(s_netif, g_set.hostname);   /* DHCP: from the next (re)connect */
#if CONFIG_KTSN_MDNS
    if (s_mdns_on) mdns_hostname_set(g_set.hostname);              /* mDNS: immediately */
#endif
}

void net_get_status(net_status_t *st)
{
    memset(st, 0, sizeof *st);
    st->connected = s_have_ip;
    st->ntp_running = s_sntp_on;
    st->ntp_synced = s_synced;
    st->last_sync = s_last_sync;
    if (s_have_ip) {
        memcpy(st->ip, s_ip, sizeof st->ip);
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) st->rssi = ap.rssi;
    }
}

void net_print_status(cmd_out_t *o)
{
    if (!g_set.ssid[0]) out_puts(o, "wifi: off (set with: wifi <ssid> [password])");
    else if (s_have_ip) {
        wifi_ap_record_t ap;
        int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
        out_printf(o, "wifi: connected to '%s', ip %s, rssi %d dBm\n", g_set.ssid, s_ip, rssi);
#if CONFIG_KTSN_MDNS
        out_printf(o, "mdns: %s.local\n", g_set.hostname);
#endif
    } else if (s_retries) out_printf(o, "wifi: connecting to '%s' (attempt %d, last failure reason %u)\n", g_set.ssid, s_retries + 1, s_last_reason);
    else out_printf(o, "wifi: connecting to '%s'\n", g_set.ssid);

    out_printf(o, "ntp:  server %s, ", g_set.ntp[0] ? g_set.ntp : "(none)");
    if (s_synced) {
        struct tm tm;
        localtime_r(&s_last_sync, &tm);
        out_printf(o, "last sync %04d-%02d-%02d %02d:%02d:%02d\n", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else out_puts(o, s_sntp_on ? "waiting for first sync" : "not running (needs wifi)");
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    out_printf(o, "time: %04d-%02d-%02d %02d:%02d:%02d (tz %s)\n", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
           tm.tm_hour, tm.tm_min, tm.tm_sec, g_set.tz);
}
