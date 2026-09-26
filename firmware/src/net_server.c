/*
 * net_server.c -- core1: W6300 Ethernet + rtl_tcp TCP server
 *
 *  - brings up the W6300 (static IP or DHCP, see config.h)
 *  - listens on CFG_RTLTCP_PORT
 *  - on connect: sends the 12-byte "RTL0" dongle header, then streams IQ bytes
 *    from the ring buffer (filled by core0 from the USB bulk pipe)
 *  - parses 5-byte rtl_tcp commands from the client and forwards them to core0
 */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "ota.h"

#include "config.h"
#include "rtl_tcp.h"
#include "leds.h"

#include "port_common.h"
#include "ethchip_conf.h"
#include "ethchip_spi.h"
#include "socket.h"
#include "dhcp.h"
#include "timer.h"

#define SOCK_RTLTCP   0
#define SOCK_DHCP     1

static uint8_t g_dhcp_buf[1024 * 2];
static uint8_t g_tx_chunk[CFG_TCP_CHUNK_BYTES];

static eth_NetInfo g_net_info = {
    .mac = { CFG_MAC_ADDR },
    .ip  = { CFG_IP_ADDR },
    .sn  = { CFG_IP_MASK },
    .gw  = { CFG_IP_GATEWAY },
    .dns = { CFG_IP_DNS },
    .lla = { 0xfe, 0x80, 0,0,0,0,0,0, 0x02,0x08,0xdc,0xff, 0xfe,0x11,0x09,0x01 },
    .gua = { 0 },
    .sn6 = { 0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff, 0,0,0,0,0,0,0,0 },
    .gw6 = { 0 },
    .dns6 = { 0 },
    .dhcp = CFG_USE_STATIC_IP ? NETINFO_STATIC : NETINFO_DHCP,
    .ipmode = CFG_USE_STATIC_IP ? NETINFO_STATIC_V4 : NETINFO_DHCP_V4,
};

/* 1 ms tick for the DHCP client */
static void tick_1ms(void)
{
    DHCP_time_handler();
}

static void dhcp_assign(void)
{
    getIPfromDHCP(g_net_info.ip);
    getGWfromDHCP(g_net_info.gw);
    getSNfromDHCP(g_net_info.sn);
    getDNSfromDHCP(g_net_info.dns);
    g_net_info.dhcp = NETINFO_DHCP;
    network_initialize(g_net_info);
    printf("[net] DHCP assigned %d.%d.%d.%d\n", g_net_info.ip[0], g_net_info.ip[1], g_net_info.ip[2], g_net_info.ip[3]);
}

static void dhcp_conflict(void)
{
    printf("[net] DHCP IP conflict!\n");
}

static void push_cmd(uint8_t cmd, uint32_t param)
{
    rtltcp_cmd_t c = { .cmd = cmd, .param = param };
    if (!queue_try_add(&g_shared.cmd_queue, &c)) {
        printf("[net] command queue full, dropped cmd %02x\n", cmd);
    }
}

static void eth_bringup(void)
{
    ethchip_spi_initialize();
    ethchip_cris_initialize();
    ethchip_reset();

    /* register callbacks + chip init (does not wait for the PHY link) */
    ethchip_initialize_nowait();
    ethchip_check();
    ethchip_1ms_timer_initialize(tick_1ms);

    if (g_net_info.dhcp == NETINFO_DHCP) {
        printf("[net] DHCP mode\n");
    } else {
        printf("[net] static IP %d.%d.%d.%d\n", g_net_info.ip[0], g_net_info.ip[1], g_net_info.ip[2], g_net_info.ip[3]);
        network_initialize(g_net_info);
        memcpy(g_shared.ip, g_net_info.ip, 4);
        g_shared.ip_ready = true;
    }
}

static bool phy_link_up(void)
{
    uint8_t tmp = PHY_LINK_OFF;
    if (ctlethchip(CW_GET_PHYLINK, (void *)&tmp) == -1) return false;
    return tmp == PHY_LINK_ON;
}

static void dhcp_poll(void)
{
    static bool started = false;
    static uint32_t retry = 0;
    if (!started) {
        DHCP_init(SOCK_DHCP, g_dhcp_buf);
        reg_dhcp_cbfunc(dhcp_assign, dhcp_assign, dhcp_conflict);
        started = true;
    }
    uint8_t r = DHCP_run();
    if (r == DHCP_IP_LEASED) {
        if (!g_shared.ip_ready) {
            memcpy(g_shared.ip, g_net_info.ip, 4);
            g_shared.ip_ready = true;
        }
    } else if (r == DHCP_FAILED) {
        retry++;
        printf("[net] DHCP failed (%lu), retrying\n", (unsigned long)retry);
        DHCP_stop();
        started = false;
    }
}

/* send the 12 byte dongle info header */
static bool send_header(void)
{
    uint8_t hdr[12];
    uint32_t t = g_shared.tuner_type, n = g_shared.tuner_gain_count;
    memcpy(hdr, "RTL0", 4);
    hdr[4] = t >> 24; hdr[5] = t >> 16; hdr[6] = t >> 8; hdr[7] = t;
    hdr[8] = n >> 24; hdr[9] = n >> 16; hdr[10] = n >> 8; hdr[11] = n;
    return send(SOCK_RTLTCP, hdr, sizeof(hdr)) == sizeof(hdr);
}

static void rtltcp_server_poll(void)
{
    static bool established = false;
    static uint8_t cmdbuf[5];
    static uint8_t cmdlen = 0;

    uint8_t sr = getSn_SR(SOCK_RTLTCP);
    switch (sr) {
    case SOCK_ESTABLISHED:
        if (!established) {
            established = true;
            cmdlen = 0;
            uint8_t dip[4] = {0, 0, 0, 0};
            getSn_DIPR(SOCK_RTLTCP, dip);
            printf("[net] client connected from %d.%d.%d.%d\n", dip[0], dip[1], dip[2], dip[3]);
            if (!send_header()) {
                disconnect(SOCK_RTLTCP);
                break;
            }
            g_shared.clients_total++;
            g_shared.client_connected = true;
            push_cmd(RTLTCP_EVT_CLIENT_CONNECTED, 0);
        }

        /* incoming commands */
        {
            uint16_t rsr = getSn_RX_RSR(SOCK_RTLTCP);
            while (rsr > 0) {
                uint8_t b;
                int32_t n = recv(SOCK_RTLTCP, &b, 1);
                if (n != 1) break;
                rsr--;
                cmdbuf[cmdlen++] = b;
                if (cmdlen == 5) {
                    uint32_t param = ((uint32_t)cmdbuf[1] << 24) | ((uint32_t)cmdbuf[2] << 16) |
                                     ((uint32_t)cmdbuf[3] << 8) | cmdbuf[4];
                    push_cmd(cmdbuf[0], param);
                    cmdlen = 0;
                }
            }
        }

        /* outgoing IQ data */
        {
            uint16_t free = getSn_TX_FSR(SOCK_RTLTCP);
            uint32_t avail = ring_used(&g_shared.iq_ring);
            uint32_t n = avail;
            if (n > free) n = free;
            if (n > sizeof(g_tx_chunk)) n = sizeof(g_tx_chunk);
            if (n >= 64 || (n > 0 && avail == n && free >= n)) {
                n = ring_read(&g_shared.iq_ring, g_tx_chunk, n);
                int32_t s = send(SOCK_RTLTCP, g_tx_chunk, (uint16_t)n);
                if (s <= 0) {
                    printf("[net] send failed (%ld), closing\n", (long)s);
                    disconnect(SOCK_RTLTCP);
                } else {
                    g_shared.tcp_bytes_total += (uint32_t)s;
                }
            }
        }
        break;

    case SOCK_CLOSE_WAIT:
        disconnect(SOCK_RTLTCP);
        break;

    case SOCK_INIT:
        listen(SOCK_RTLTCP);
        break;

    case SOCK_CLOSED:
        if (established) {
            established = false;
            g_shared.client_connected = false;
            push_cmd(RTLTCP_EVT_CLIENT_DISCONNECTED, 0);
            printf("[net] client disconnected\n");
        }
        socket(SOCK_RTLTCP, Sn_MR_TCP4, CFG_RTLTCP_PORT, SF_TCP_NODELAY);
        break;

    case SOCK_LISTEN:
    default:
        break;
    }
}

void net_core1_main(void)
{
    printf("[net] core1 start\n");
    eth_bringup();

    absolute_time_t next_link_check = get_absolute_time();
    bool link = false;

    while (true) {
        if (time_reached(next_link_check)) {
            next_link_check = make_timeout_time_ms(500);
            bool l = phy_link_up();
            if (l != link) {
                link = l;
                g_shared.link_up = l;
                printf("[net] PHY link %s\n", l ? "UP" : "DOWN");
                if (!l) {
                    g_shared.ip_ready = (g_net_info.dhcp != NETINFO_DHCP);
                }
            }
        }
        if (!link) {
            sleep_ms(10);
            continue;
        }
        if (g_net_info.dhcp == NETINFO_DHCP) {
            dhcp_poll();
            if (!g_shared.ip_ready) { sleep_ms(1); continue; }
        }
        ota_poll();
        rtltcp_server_poll();
    }
}
