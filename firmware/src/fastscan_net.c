/*
 * fastscan_net.c -- network side of the 改良版1 fast scanner (core1)
 *   TCP 1237      : text control (HELP / ADD / LIST / ...) and live HIT / END lines (WATCH)
 *   UDP 1238      : coordination between units ("MON <id> <hz>" / "END <id> <hz>", broadcast)
 *   UDP 1240+id   : AM audio, 12500 Hz signed 16 bit little endian mono.  A listener that
 *                   sends "HELLO" to this port gets the audio (host/fastscan_listen.sh)
 */
#include "config.h"
#if CFG_FASTSCAN

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "socket.h"
#include "fastscan.h"

#define SOCK_CTRL   4
#define SOCK_AUDIO  5
#define SOCK_COORD  6
#define AUDIO_PKT   500            /* 250 samples = 20 ms */
#define LISTENER_TIMEOUT_MS 60000u /* a HELLO keeps the audio coming this long */
#define DEAD_HOLDOFF_MS     10000u /* destination did not answer ARP: stop sending for a while */

static char     s_line[160];
static uint16_t s_line_len;
static bool     s_ctrl_est;
static bool     s_watch;

static char     s_ev[128];         /* one event line on its way to the control client */
static uint16_t s_ev_len;
static bool     s_ev_ready;

static uint8_t  s_lis_ip[4];       /* last HELLO */
static uint32_t s_lis_ms;
static bool     s_lis_valid;
static uint8_t  s_dead_ip[4];
static uint32_t s_dead_until;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

static void ctrl_emit(const char *s)
{
    uint16_t len = (uint16_t)strlen(s);
    if (len) send(SOCK_CTRL, (uint8_t *)s, len);
}

/* commands handled per connection; everything else goes to fastscan_ctl().  false = close */
static bool ctrl_line(char *line)
{
    char w[8] = { 0 };
    for (int i = 0; i < 7 && line[i] && line[i] != ' '; i++) w[i] = (char)((line[i] >= 'a' && line[i] <= 'z') ? line[i] - 32 : line[i]);
    if (!strcmp(w, "QUIT")) {
        ctrl_emit("BYE\n");
        return false;
    }
    if (!strcmp(w, "WATCH")) {
        const char *a = line + 5;
        while (*a == ' ' || *a == '\t') a++;
        if (!strncasecmp(a, "ON", 2)) s_watch = true;
        else if (!strncasecmp(a, "OFF", 3)) s_watch = false;
        else { ctrl_emit("ERR WATCH ON|OFF\n"); return true; }
        ctrl_emit("OK\n");
        return true;
    }
    fastscan_ctl(line, ctrl_emit);
    return true;
}

/* run every complete line received so far (also after the client half-closed: nc < file) */
static bool ctrl_read(bool closing)
{
    uint16_t rsr = getSn_RX_RSR(SOCK_CTRL);
    while (rsr > 0) {
        uint8_t c;
        if (recv(SOCK_CTRL, &c, 1) != 1) break;
        rsr--;
        if (c == '\n' || c == '\r') {
            if (s_line_len) {
                s_line[s_line_len] = 0;
                s_line_len = 0;
                if (!ctrl_line(s_line)) return false;
            }
        } else if (s_line_len < sizeof(s_line) - 1) {
            s_line[s_line_len++] = (char)c;
        }
    }
    if (closing && s_line_len) {               /* last line without a newline */
        s_line[s_line_len] = 0;
        s_line_len = 0;
        ctrl_line(s_line);
    }
    return true;
}

static void ctrl_poll(void)
{
    uint8_t sr = getSn_SR(SOCK_CTRL);
    switch (sr) {
    case SOCK_CLOSED:
        s_ctrl_est = false;
        socket(SOCK_CTRL, Sn_MR_TCP4, FSCAN_CTRL_PORT, 0);
        break;
    case SOCK_INIT:
        listen(SOCK_CTRL);
        break;
    case SOCK_CLOSE_WAIT:
        if (s_ctrl_est) ctrl_read(true);
        disconnect(SOCK_CTRL);
        break;
    case SOCK_ESTABLISHED: {
        if (!s_ctrl_est) {
            s_ctrl_est = true;
            s_watch = true;
            s_line_len = 0;
            uint8_t ip[4];
            getSn_DIPR(SOCK_CTRL, ip);
            fastscan_set_client_ip(ip);
            char b[120];
            snprintf(b, sizeof(b), "RP2350 fastscan (改良版1) board %d ready (HELP for commands, WATCH OFF to mute HIT/END)\n",
                     CFG_BOARD_ID);
            ctrl_emit(b);
            printf("[ctl] client %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3]);
        }
        if (!ctrl_read(false)) disconnect(SOCK_CTRL);
        break;
    }
    default:
        break;
    }
}

/* HIT / END lines: to the control client if it watches, otherwise dropped */
static void events_poll(void)
{
    ring_t *ev = fastscan_event_ring();
    while (!s_ev_ready && ring_used(ev)) {
        uint8_t c;
        ring_read(ev, &c, 1);
        s_ev[s_ev_len++] = (char)c;
        if (c == '\n' || s_ev_len == sizeof(s_ev)) s_ev_ready = true;
    }
    if (!s_ev_ready) return;
    bool listening = s_ctrl_est && s_watch && getSn_SR(SOCK_CTRL) == SOCK_ESTABLISHED;
    if (listening) {
        if (getSn_TX_FSR(SOCK_CTRL) < s_ev_len) return;   /* try again later, never block here */
        send(SOCK_CTRL, (uint8_t *)s_ev, s_ev_len);
    }
    s_ev_len = 0;
    s_ev_ready = false;
}

static bool audio_dest(uint8_t ip[4], uint16_t *port)
{
    *port = FSCAN_AUDIO_PORT_BASE + CFG_BOARD_ID;
    if (fastscan_audio_auto() && s_lis_valid && now_ms() - s_lis_ms < LISTENER_TIMEOUT_MS) {
        memcpy(ip, s_lis_ip, 4);
        return true;
    }
    return fastscan_audio_dest(ip, port);
}

static void audio_poll(void)
{
    static uint8_t pkt[AUDIO_PKT];
    if (getSn_SR(SOCK_AUDIO) != SOCK_UDP) {
        close(SOCK_AUDIO);
        socket(SOCK_AUDIO, Sn_MR_UDP4, FSCAN_AUDIO_PORT_BASE + CFG_BOARD_ID, 0);
        return;
    }
    /* listeners announce themselves */
    while (getSn_RX_RSR(SOCK_AUDIO) > 0) {
        uint8_t b[32], ip[4], alen = 4;
        uint16_t port;
        int32_t n = recvfrom(SOCK_AUDIO, b, sizeof(b), ip, &port, &alen);
        if (n <= 0) break;
        if (n >= 5 && !memcmp(b, "HELLO", 5)) {
            if (!s_lis_valid || memcmp(ip, s_lis_ip, 4) || now_ms() - s_lis_ms >= LISTENER_TIMEOUT_MS)
                printf("[fscan] audio listener %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3]);
            memcpy(s_lis_ip, ip, 4);
            s_lis_ms = now_ms();
            s_lis_valid = true;
            if (!memcmp(ip, s_dead_ip, 4)) s_dead_until = s_lis_ms;   /* it is alive again */
        }
    }
    ring_t *r = fastscan_audio_ring();
    if (ring_used(r) < AUDIO_PKT) return;
    uint8_t ip[4];
    uint16_t port;
    if (!audio_dest(ip, &port)) { ring_reset(r); return; }
    ring_read(r, pkt, AUDIO_PKT);
    if (!memcmp(ip, s_dead_ip, 4) && (int32_t)(s_dead_until - now_ms()) > 0) return;
    /* sendto() blocks until the ARP reply: a listener that went away would stall core1 */
    if (sendto(SOCK_AUDIO, pkt, AUDIO_PKT, ip, port, 4) == SOCKERR_TIMEOUT) {
        printf("[fscan] audio to %u.%u.%u.%u timed out, pausing %lu s\n", ip[0], ip[1], ip[2], ip[3],
               (unsigned long)(DEAD_HOLDOFF_MS / 1000));
        memcpy(s_dead_ip, ip, 4);
        s_dead_until = now_ms() + DEAD_HOLDOFF_MS;
    }
}

void fastscan_net_status(fscan_emit_fn emit)
{
    char b[120];
    uint8_t ip[4];
    uint16_t port;
    bool listener = fastscan_audio_auto() && s_lis_valid && now_ms() - s_lis_ms < LISTENER_TIMEOUT_MS;
    if (audio_dest(ip, &port))
        snprintf(b, sizeof(b), "audio -> %u.%u.%u.%u:%u (%s)  12500 Hz s16le mono\n", ip[0], ip[1], ip[2], ip[3],
                 port, listener ? "listener HELLO" : fastscan_audio_auto() ? "AUTO: control client" : "fixed");
    else
        snprintf(b, sizeof(b), "audio -> none (%s)\n", fastscan_audio_auto() ? "AUTO, no listener yet" : "OFF");
    emit(b);
}

static void coord_poll(void)
{
    static absolute_time_t next_tx;
    static uint32_t last_mon = 0;
    static uint8_t bcast[4] = { 255, 255, 255, 255 };
    if (getSn_SR(SOCK_COORD) != SOCK_UDP) {
        close(SOCK_COORD);
        socket(SOCK_COORD, Sn_MR_UDP4, FSCAN_COORD_PORT, 0);
        next_tx = get_absolute_time();
        return;
    }
    /* receive */
    while (getSn_RX_RSR(SOCK_COORD) > 0) {
        char b[64];
        uint8_t ip[4];
        uint16_t port;
        uint8_t alen = 4;
        int32_t n = recvfrom(SOCK_COORD, (uint8_t *)b, sizeof(b) - 1, ip, &port, &alen);
        if (n <= 0) break;
        b[n] = 0;
        unsigned id;
        unsigned long hz;
        if (sscanf(b, "MON %u %lu", &id, &hz) == 2) fastscan_peer_mon((uint8_t)id, (uint32_t)hz);
        else if (sscanf(b, "END %u %lu", &id, &hz) == 2) fastscan_peer_end((uint8_t)id, (uint32_t)hz);
    }
    /* transmit */
    uint32_t mon = fastscan_monitor_freq();
    char b[48];
    if (mon != last_mon && last_mon) {
        snprintf(b, sizeof(b), "END %d %lu\n", CFG_BOARD_ID, (unsigned long)last_mon);
        sendto(SOCK_COORD, (uint8_t *)b, (uint16_t)strlen(b), bcast, FSCAN_COORD_PORT, 4);
    }
    if (mon && (mon != last_mon || time_reached(next_tx))) {
        snprintf(b, sizeof(b), "MON %d %lu\n", CFG_BOARD_ID, (unsigned long)mon);
        sendto(SOCK_COORD, (uint8_t *)b, (uint16_t)strlen(b), bcast, FSCAN_COORD_PORT, 4);
        next_tx = make_timeout_time_ms(200);
    }
    last_mon = mon;
}

void fastscan_net_poll(void)
{
    ctrl_poll();
    events_poll();
    audio_poll();
    coord_poll();
}

#endif /* CFG_FASTSCAN */
