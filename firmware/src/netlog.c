/* netlog.c -- mirror printf output to TCP 1236 (keeps last 8KB since boot) */
#include <string.h>
#include "pico/stdlib.h"
#include "pico/stdio/driver.h"
#include "hardware/sync.h"
#include "socket.h"
#include "netlog.h"

#define SOCK_LOG  3
#define LOG_PORT  1236
#define LOG_SIZE  8192u

static char s_log[LOG_SIZE];
static volatile uint32_t s_head;
static uint32_t s_rd;
static bool s_est;

static void netlog_out_chars(const char *buf, int len)
{
    uint32_t h = s_head;
    for (int i = 0; i < len; i++) { s_log[h % LOG_SIZE] = buf[i]; h++; }
    __dmb();
    s_head = h;
}

static stdio_driver_t s_drv = { .out_chars = netlog_out_chars };

void netlog_init(void) { stdio_set_driver_enabled(&s_drv, true); }

void netlog_poll(void)
{
    uint8_t sr = getSn_SR(SOCK_LOG);
    switch (sr) {
    case SOCK_CLOSED:
        s_est = false;
        socket(SOCK_LOG, Sn_MR_TCP4, LOG_PORT, 0);
        break;
    case SOCK_INIT:
        listen(SOCK_LOG);
        break;
    case SOCK_CLOSE_WAIT:
        disconnect(SOCK_LOG);
        break;
    case SOCK_ESTABLISHED: {
        uint32_t h = s_head;
        if (!s_est) { s_est = true; s_rd = (h > LOG_SIZE) ? h - LOG_SIZE : 0; }
        if (h - s_rd > LOG_SIZE) s_rd = h - LOG_SIZE;
        uint32_t n = h - s_rd;
        if (n == 0) break;
        uint32_t off = s_rd % LOG_SIZE;
        uint32_t contig = LOG_SIZE - off;
        if (n > contig) n = contig;
        uint16_t fsr = getSn_TX_FSR(SOCK_LOG);
        if (n > fsr) n = fsr;
        if (n > 1024) n = 1024;
        if (n == 0) break;
        int32_t s = send(SOCK_LOG, (uint8_t *)&s_log[off], (uint16_t)n);
        if (s > 0) s_rd += (uint32_t)s; else disconnect(SOCK_LOG);
        break;
    }
    default:
        break;
    }
}
