/* Network CLI: the same commands as the serial console, over TCP (telnet or netcat), one session at a time.
 * Everyone is refused until a token is set (`token <secret>`); each session must start with `auth <token>`.
 * Plain TCP, no encryption: meant for a trusted LAN. */
#include "netcli.h"
#include <stdbool.h>
#include <string.h>
#include "cmd.h"
#include "settings.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#define PORT            CONFIG_KTSN_NET_CLI_PORT
#define IDLE_TIMEOUT_S  600
#define MAX_AUTH_FAILS  3

static const char *TAG = "netcli";

/* Telnet option negotiation is ignored: strip IAC sequences (and subnegotiation) from the byte stream. */
typedef enum { T_DATA, T_IAC, T_OPT, T_SB, T_SB_IAC } tstate_t;

typedef struct {
    int      fd, lfd;
    bool     ok;             /* false once a send failed: stop writing */
    uint8_t  rx[128];        /* received bytes not yet consumed by next_line */
    int      rx_pos, rx_n;
    tstate_t st;
    bool     last_cr;
} sess_t;

static bool send_all(sess_t *ss, const char *p, size_t n)
{
    while (n && ss->ok) {
        int r = send(ss->fd, p, n, 0);
        if (r <= 0) { ss->ok = false; break; }
        p += r; n -= (size_t)r;
    }
    return ss->ok;
}

/* Command output: "\n" becomes "\r\n" for telnet (netcat doesn't mind). */
static void sock_write(cmd_out_t *o, const char *s, size_t n)
{
    sess_t *ss = o->ctx;
    const char *end = s + n;
    while (s < end && ss->ok) {
        const char *nl = memchr(s, '\n', (size_t)(end - s));
        size_t chunk = nl ? (size_t)(nl - s) : (size_t)(end - s);
        if (chunk) send_all(ss, s, chunk);
        if (!nl) break;
        send_all(ss, "\r\n", 2);
        s = nl + 1;
    }
}

static bool token_set(void)
{
    cmd_lock();
    bool set = g_set.token[0] != 0;
    cmd_unlock();
    return set;
}

/* The next input line: telnet commands stripped, \r\n counts once, backspace edits. Turns away other clients while
 * waiting. Returns its length, or CMD_READ_CLOSED (closed, Ctrl-D, idle timeout), CMD_READ_CANCEL (Ctrl-C) or
 * CMD_READ_TOO_LONG. Used for commands and, through cmd_out_t.read_line, for multi-line entry. */
static int next_line(sess_t *ss, char *line, size_t cap)
{
    size_t len = 0;
    bool overflow = false;
    while (ss->ok) {
        if (ss->rx_pos == ss->rx_n) {
            fd_set rd;
            FD_ZERO(&rd); FD_SET(ss->fd, &rd); FD_SET(ss->lfd, &rd);
            struct timeval idle = { .tv_sec = IDLE_TIMEOUT_S };
            int n = select((ss->fd > ss->lfd ? ss->fd : ss->lfd) + 1, &rd, NULL, NULL, &idle);
            if (n == 0) { send_all(ss, "\r\nidle timeout\r\n", 18); return CMD_READ_CLOSED; }
            if (n < 0) return CMD_READ_CLOSED;
            if (FD_ISSET(ss->lfd, &rd)) {                 /* someone else knocks: tell them, keep our session */
                int other = accept(ss->lfd, NULL, NULL);
                if (other >= 0) { static const char busy[] = "KTSN LED: busy (another session is open)\r\n"; send(other, busy, sizeof busy - 1, 0); close(other); }
            }
            if (!FD_ISSET(ss->fd, &rd)) continue;
            int r = recv(ss->fd, ss->rx, sizeof ss->rx, 0);
            if (r <= 0) return CMD_READ_CLOSED;
            ss->rx_pos = 0; ss->rx_n = r;
        }
        uint8_t b = ss->rx[ss->rx_pos++];
        switch (ss->st) {
        case T_IAC:    ss->st = (b >= 251 && b <= 254) ? T_OPT : b == 250 ? T_SB : T_DATA; continue;
        case T_OPT:    ss->st = T_DATA; continue;
        case T_SB:     if (b == 255) ss->st = T_SB_IAC; continue;
        case T_SB_IAC: ss->st = b == 240 ? T_DATA : T_SB; continue;
        case T_DATA:   if (b == 255) { ss->st = T_IAC; continue; } break;
        }
        if (b == '\n' && ss->last_cr) { ss->last_cr = false; continue; }
        ss->last_cr = b == '\r';
        if (b == 4) return CMD_READ_CLOSED;               /* ctrl-D */
        if (b == 3) { send_all(ss, "^C\r\n", 4); return CMD_READ_CANCEL; }
        if (b == 8 || b == 127) { if (len) len--; continue; }
        if (b != '\r' && b != '\n') {
            if (b < 32 && b != '\t') continue;
            if (len < cap - 1) line[len++] = (char)b; else overflow = true;
            continue;
        }
        line[len] = 0;
        return overflow ? CMD_READ_TOO_LONG : (int)len;
    }
    return CMD_READ_CLOSED;
}

static int sock_read_line(cmd_out_t *o, char *buf, size_t cap) { return next_line(o->ctx, buf, cap); }

static void session(int lfd, int fd, const char *peer)
{
    sess_t ss = { .fd = fd, .lfd = lfd, .ok = true };
    cmd_out_t o = { .write = sock_write, .read_line = sock_read_line, .ctx = &ss };
    struct timeval tv = { .tv_sec = 5 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);   /* a stalled client can't block us forever */

    if (!token_set()) {
        out_puts(&o, "KTSN LED: network access is disabled until a token is set on the serial console: token <secret>");
        return;
    }
    ESP_LOGI(TAG, "session from %s", peer);
    out_puts(&o, "KTSN LED: authenticate with: auth <token>   (exit to quit)");

    static char line[CMD_LINE_MAX];                       /* one session at a time: static keeps it off the stack */
    bool authed = false;
    int fails = 0;
    while (ss.ok) {
        send_all(&ss, "led> ", 5);
        int n = next_line(&ss, line, sizeof line);
        if (n == CMD_READ_CLOSED) return;
        if (n == CMD_READ_CANCEL) continue;
        if (n == CMD_READ_TOO_LONG) { out_printf(&o, "line too long (max %d characters)\n", CMD_LINE_MAX - 1); continue; }
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!strcmp(p, "exit") || !strcmp(p, "quit")) return;
        if (!strncmp(p, "auth ", 5) || !strcmp(p, "auth")) {
            if (authed) out_puts(&o, "already authenticated");
            else if (settings_token_ok(p[4] ? p + 5 : "")) { authed = true; out_puts(&o, "ok"); ESP_LOGI(TAG, "%s authenticated", peer); }
            else {
                ESP_LOGW(TAG, "%s: failed authentication", peer);
                vTaskDelay(pdMS_TO_TICKS(1000));          /* slow down guessing */
                if (++fails >= MAX_AUTH_FAILS) { out_puts(&o, "authentication failed, closing"); return; }
                out_puts(&o, "authentication failed");
            }
            memset(line, 0, sizeof line);
        } else if (*p && !authed) {
            out_puts(&o, "not authenticated: auth <token>");
        } else if (*p) {
            cmd_exec(&o, p);
        }
    }
}

static void server_task(void *arg)
{
    (void)arg;
    int lfd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    int one = 1;
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (lfd < 0 || setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) ||
        bind(lfd, (struct sockaddr *)&addr, sizeof addr) || listen(lfd, 2)) {
        ESP_LOGE(TAG, "can't listen on port %d", PORT);
        if (lfd >= 0) close(lfd);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "listening on port %d", PORT);
    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof peer;
        int fd = accept(lfd, (struct sockaddr *)&peer, &plen);
        if (fd < 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        char ip[16];
        inet_ntoa_r(peer.sin_addr, ip, sizeof ip);
        session(lfd, fd, ip);
        shutdown(fd, SHUT_RDWR);
        close(fd);
        ESP_LOGI(TAG, "session from %s closed", ip);
    }
}

void netcli_start(void)
{
    static bool started;
    if (started) return;
    started = true;
    /* Core 0 with WiFi, below the render task (core 1). Commands compile programs here, so give it room. */
    xTaskCreatePinnedToCore(server_task, "netcli", 8192, NULL, 3, NULL, 0);
}
