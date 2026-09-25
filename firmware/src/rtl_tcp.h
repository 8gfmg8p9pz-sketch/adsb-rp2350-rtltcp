/* shared definitions between core0 (USB/SDR) and core1 (network) */
#ifndef RTL_TCP_H
#define RTL_TCP_H

#include <stdint.h>
#include <stdbool.h>
#include "pico/util/queue.h"
#include "ring.h"

/* rtl_tcp wire command: 1 byte cmd + 4 byte big-endian parameter */
typedef struct {
    uint8_t  cmd;
    uint32_t param;   /* already converted to host order */
} rtltcp_cmd_t;

enum {
    RTLTCP_CMD_SET_FREQ           = 0x01,
    RTLTCP_CMD_SET_SAMPLE_RATE    = 0x02,
    RTLTCP_CMD_SET_GAIN_MODE      = 0x03,
    RTLTCP_CMD_SET_GAIN           = 0x04,
    RTLTCP_CMD_SET_FREQ_CORR      = 0x05,
    RTLTCP_CMD_SET_IF_GAIN        = 0x06,
    RTLTCP_CMD_SET_TEST_MODE      = 0x07,
    RTLTCP_CMD_SET_AGC_MODE       = 0x08,
    RTLTCP_CMD_SET_DIRECT_SAMPLING= 0x09,
    RTLTCP_CMD_SET_OFFSET_TUNING  = 0x0a,
    RTLTCP_CMD_SET_RTL_XTAL       = 0x0b,
    RTLTCP_CMD_SET_TUNER_XTAL     = 0x0c,
    RTLTCP_CMD_SET_GAIN_BY_INDEX  = 0x0d,
    RTLTCP_CMD_SET_BIAS_TEE       = 0x0e,
    /* internal pseudo commands (core1 -> core0) */
    RTLTCP_EVT_CLIENT_CONNECTED   = 0xf0,
    RTLTCP_EVT_CLIENT_DISCONNECTED= 0xf1,
};

typedef struct {
    queue_t  cmd_queue;               /* core1 -> core0 */
    ring_t   iq_ring;                 /* core0 -> core1 */
    volatile bool     client_connected;
    volatile bool     sdr_ready;      /* core0: dongle initialised */
    volatile bool     link_up;        /* core1: PHY link */
    volatile bool     ip_ready;       /* core1: IP configured */
    volatile uint32_t tuner_type;     /* for the "RTL0" header */
    volatile uint32_t tuner_gain_count;
    volatile uint32_t tcp_bytes_total;
    volatile uint32_t clients_total;
    uint8_t  ip[4];
} rtltcp_shared_t;

extern rtltcp_shared_t g_shared;

/* core1 entry point (never returns) */
void net_core1_main(void);

#endif
