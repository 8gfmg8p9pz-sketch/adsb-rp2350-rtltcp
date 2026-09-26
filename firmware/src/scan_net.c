/*
 * scan_net.c -- network side of the scanner (core1)
 *   TCP 1237      : text control (HELP / ADD / DEL / LIST / SQL / SAVE ...)
 *   UDP 1238      : coordination between units ("MON <id> <hz>" / "END <id> <hz>", broadcast)
 *   UDP 1240+id   : AM audio, 12500 Hz signed 16 bit little endian mono
 */
#include "config.h"
#if CFG_SCANNER

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "socket.h"
#include "scanner.h"

#define SOCK_CTRL   4
#define SOCK_AUDIO  5
#define SOCK_COORD  6
#define AUDIO_PKT   500            /* 250 samples = 20 ms */

static char     s_line[160];
static uint16_t s_line_len;
static bool     s_ctrl_est;

static void ctrl_emit(const char *s)
{
    uint16_t len = (uint16_t)strlen(s);
    if (len) send(SOCK_CTRL, (uint8_t *)s, len);
}

static void ctrl_poll(void)
{
    uint8_t sr = getSn_SR(SOCK_CTRL);
    switch (sr) {
    case SOCK_CLOSED:
        s_ctrl_est = false;
        socket(SOCK_CTRL, Sn_MR_TCP4, SCAN_CTRL_PORT, 0);
        break;
    case SOCK_INIT:
        listen(SOCK_CTRL);
        break;
    case SOCK_CLOSE_WAIT:
        disconnect(SOCK_CTRL);
        break;
    case SOCK_ESTABLISHED: {
        if (!s_ctrl_est) {
            s_ctrl_est = true;
            s_line_len = 0;
            uint8_t ip[4];
            getSn_DIPR(SOCK_CTRL, ip);
            scanner_set_client_ip(ip);
            char b[96];
            snprintf(b, sizeof(b), "RP2350-SCAN board %d ready (HELP for commands)\n", CFG_BOARD_ID);
            ctrl_emit(b);
            printf("[ctl] client %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3]);
        }
        uint16_t rsr = getSn_RX_RSR(SOCK_CTRL);
        while (rsr > 0) {
            uint8_t c;
            if (recv(SOCK_CTRL, &c, 1) != 1) break;
            rsr--;
            if (c == '\n' || c == '\r') {
                if (s_line_len) {
                    s_line[s_line_len] = 0;
                    scanner_ctl(s_line, ctrl_emit);
                    s_line_len = 0;
                }
            } else if (s_line_len < sizeof(s_line) - 1) {
                s_line[s_line_len++] = (char)c;
            }
        }
        break;
    }
    default:
        break;
    }
}

static void audio_poll(void)
{
    static uint8_t pkt[AUDIO_PKT];
    if (getSn_SR(SOCK_AUDIO) != SOCK_UDP) {
        close(SOCK_AUDIO);
        socket(SOCK_AUDIO, Sn_MR_UDP4, SCAN_AUDIO_PORT_BASE + CFG_BOARD_ID, 0);
        return;
    }
    ring_t *r = scanner_audio_ring();
    if (ring_used(r) < AUDIO_PKT) return;
    uint8_t ip[4];
    uint16_t port;
    if (!scanner_audio_dest(ip, &port)) { ring_reset(r); return; }
    ring_read(r, pkt, AUDIO_PKT);
    sendto(SOCK_AUDIO, pkt, AUDIO_PKT, ip, port, 4);
}

static void coord_poll(void)
{
    static absolute_time_t next_tx;
    static uint32_t last_mon = 0;
    static uint8_t bcast[4] = { 255, 255, 255, 255 };
    if (getSn_SR(SOCK_COORD) != SOCK_UDP) {
        close(SOCK_COORD);
        socket(SOCK_COORD, Sn_MR_UDP4, SCAN_COORD_PORT, 0);
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
        if (sscanf(b, "MON %u %lu", &id, &hz) == 2) scanner_peer_mon((uint8_t)id, (uint32_t)hz);
        else if (sscanf(b, "END %u %lu", &id, &hz) == 2) scanner_peer_end((uint8_t)id, (uint32_t)hz);
    }
    /* transmit */
    uint32_t mon = scanner_monitor_freq();
    char b[48];
    if (mon != last_mon && last_mon) {
        snprintf(b, sizeof(b), "END %d %lu\n", CFG_BOARD_ID, (unsigned long)last_mon);
        sendto(SOCK_COORD, (uint8_t *)b, (uint16_t)strlen(b), bcast, SCAN_COORD_PORT, 4);
    }
    if (mon && (mon != last_mon || time_reached(next_tx))) {
        snprintf(b, sizeof(b), "MON %d %lu\n", CFG_BOARD_ID, (unsigned long)mon);
        sendto(SOCK_COORD, (uint8_t *)b, (uint16_t)strlen(b), bcast, SCAN_COORD_PORT, 4);
        next_tx = make_timeout_time_ms(200);
    }
    last_mon = mon;
}

void scan_net_poll(void)
{
    ctrl_poll();
    audio_poll();
    coord_poll();
}

#endif /* CFG_SCANNER */
