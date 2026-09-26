/*
 * main.c -- RP2350-POE-ETH rtl_tcp server
 *
 *  core0 : TinyUSB host (RTL2832U), rtl_tcp command execution, IQ ring producer
 *  core1 : W6300 Ethernet, rtl_tcp TCP server, IQ ring consumer   (net_server.c)
 */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"

#include "config.h"
#include "rtl_tcp.h"
#include "usb_host.h"
#include "rtl2832u.h"
#include "leds.h"

rtltcp_shared_t g_shared;
static uint8_t g_ring_storage[CFG_RING_BUFFER_BYTES];

#define VERSION_STR "1.0.0"

/* ------------------------------------------------------------------------- */
static void debug_uart_init(void)
{
    uart_init(uart0, CFG_DEBUG_UART_BAUD);
    gpio_set_function(CFG_DEBUG_UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(CFG_DEBUG_UART_RX_PIN, GPIO_FUNC_UART);
    stdio_uart_init_full(uart0, CFG_DEBUG_UART_BAUD, CFG_DEBUG_UART_TX_PIN, CFG_DEBUG_UART_RX_PIN);
}

static void clocks_setup(void)
{
    /* 150 MHz system clock, peripherals (SPI for the W6300) from PLL_SYS */
    set_sys_clock_khz(150000, true);
    clock_configure(clk_peri, 0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    150000 * 1000, 150000 * 1000);
}

/* apply config.h defaults after the dongle has been opened */
static void apply_defaults(void)
{
    rtlsdr_set_sample_rate(CFG_DEFAULT_SAMPLE_RATE);
    rtlsdr_set_center_freq(CFG_DEFAULT_FREQ_HZ);
    rtlsdr_set_freq_correction(CFG_DEFAULT_PPM);
    if (CFG_DEFAULT_AGC) {
        rtlsdr_set_tuner_gain_mode(0);
        rtlsdr_set_agc_mode(1);
    } else {
        rtlsdr_set_tuner_gain_mode(1);
        rtlsdr_set_tuner_gain(CFG_DEFAULT_GAIN_TENTHDB);
    }
    if (CFG_DEFAULT_BIAS_TEE) rtlsdr_set_bias_tee(1);
}

static void execute_cmd(const rtltcp_cmd_t *c)
{
    uint32_t p = c->param;
    switch (c->cmd) {
    case RTLTCP_CMD_SET_FREQ:
        printf("[cmd] set freq %lu\n", (unsigned long)p);
        rtlsdr_set_center_freq(p);
        break;
    case RTLTCP_CMD_SET_SAMPLE_RATE:
#if CFG_LIMIT_SAMPLE_RATE
        if (p > CFG_MAX_SAMPLE_RATE) {
            printf("[cmd] sample rate %lu limited to %lu\n", (unsigned long)p, (unsigned long)CFG_MAX_SAMPLE_RATE);
            p = CFG_MAX_SAMPLE_RATE;
        }
#endif
        printf("[cmd] set sample rate %lu\n", (unsigned long)p);
        rtlsdr_set_sample_rate(p);
        break;
    case RTLTCP_CMD_SET_GAIN_MODE:
        printf("[cmd] set gain mode %lu\n", (unsigned long)p);
        rtlsdr_set_tuner_gain_mode((int)p);
        break;
    case RTLTCP_CMD_SET_GAIN:
        printf("[cmd] set gain %lu\n", (unsigned long)p);
        rtlsdr_set_tuner_gain((int)p);
        break;
    case RTLTCP_CMD_SET_FREQ_CORR:
        printf("[cmd] set freq correction %ld\n", (long)(int32_t)p);
        rtlsdr_set_freq_correction((int)(int32_t)p);
        break;
    case RTLTCP_CMD_SET_IF_GAIN:
        printf("[cmd] set if stage %lu gain %d\n", (unsigned long)(p >> 16), (short)(p & 0xffff));
        rtlsdr_set_tuner_if_gain((int)(p >> 16), (short)(p & 0xffff));
        break;
    case RTLTCP_CMD_SET_TEST_MODE:
        printf("[cmd] set test mode %lu\n", (unsigned long)p);
        rtlsdr_set_testmode((int)p);
        break;
    case RTLTCP_CMD_SET_AGC_MODE:
        printf("[cmd] set agc mode %lu\n", (unsigned long)p);
        rtlsdr_set_agc_mode((int)p);
        break;
    case RTLTCP_CMD_SET_DIRECT_SAMPLING:
        printf("[cmd] set direct sampling %lu\n", (unsigned long)p);
        rtlsdr_set_direct_sampling((int)p);
        break;
    case RTLTCP_CMD_SET_OFFSET_TUNING:
        printf("[cmd] set offset tuning %lu\n", (unsigned long)p);
        rtlsdr_set_offset_tuning((int)p);
        break;
    case RTLTCP_CMD_SET_RTL_XTAL:
        printf("[cmd] set rtl xtal %lu\n", (unsigned long)p);
        rtlsdr_set_xtal_freq(p, 0);
        break;
    case RTLTCP_CMD_SET_TUNER_XTAL:
        printf("[cmd] set tuner xtal %lu\n", (unsigned long)p);
        rtlsdr_set_xtal_freq(0, p);
        break;
    case RTLTCP_CMD_SET_GAIN_BY_INDEX:
        printf("[cmd] set tuner gain by index %lu\n", (unsigned long)p);
        rtlsdr_set_tuner_gain_by_index(p);
        break;
    case RTLTCP_CMD_SET_BIAS_TEE:
        printf("[cmd] set bias tee %lu\n", (unsigned long)p);
        rtlsdr_set_bias_tee((int)p);
        break;
    default:
        printf("[cmd] unknown command 0x%02x\n", c->cmd);
        break;
    }
}

static void print_stats(void)
{
    static uint32_t last_usb = 0, last_tcp = 0;
    uint32_t usb = usb_stream_bytes_total();
    uint32_t tcp = g_shared.tcp_bytes_total;
    printf("[stat] usb %lu kB/s  tcp %lu kB/s  ring %lu/%lu  overrun %lu  usberr %lu  client %d\n",
           (unsigned long)((usb - last_usb) / 1024 / CFG_STATUS_PRINT_SEC),
           (unsigned long)((tcp - last_tcp) / 1024 / CFG_STATUS_PRINT_SEC),
           (unsigned long)ring_used(&g_shared.iq_ring), (unsigned long)CFG_RING_BUFFER_BYTES,
           (unsigned long)g_shared.iq_ring.overruns, (unsigned long)usb_stream_errors(),
           g_shared.client_connected);
    last_usb = usb;
    last_tcp = tcp;
}

/* ------------------------------------------------------------------------- */
int main(void)
{
    clocks_setup();
    debug_uart_init();
    sleep_ms(200);
    printf("\n\n=== RP2350-POE-ETH rtl_tcp server v%s ===\n", VERSION_STR);
    printf("port %u, default %lu Hz @ %lu S/s\n", CFG_RTLTCP_PORT,
           (unsigned long)CFG_DEFAULT_FREQ_HZ, (unsigned long)CFG_DEFAULT_SAMPLE_RATE);

    leds_init();

    memset(&g_shared, 0, sizeof(g_shared));
    queue_init(&g_shared.cmd_queue, sizeof(rtltcp_cmd_t), 32);
    ring_init(&g_shared.iq_ring, g_ring_storage, sizeof(g_ring_storage));
    usb_stream_set_ring(&g_shared.iq_ring);

    /* network on core1 */
    multicore_launch_core1(net_core1_main);
    multicore_lockout_victim_init();

    /* USB host on core0 */
    usb_host_init();
    printf("[usb] host started, waiting for RTL-SDR...\n");

    absolute_time_t next_stats = make_timeout_time_ms(CFG_STATUS_PRINT_SEC * 1000);
    bool streaming = false;

    while (true) {
        usb_host_task();

        /* dongle just enumerated -> initialise it */
        if (usb_host_state() == USBH_DEVICE_READY) {
            printf("[rtl] initialising RTL2832U...\n");
            if (rtlsdr_open() == 0 && rtlsdr_get_tuner_type() != RTLSDR_TUNER_UNKNOWN) {
                apply_defaults();
                g_shared.tuner_type = (uint32_t)rtlsdr_get_tuner_type();
                g_shared.tuner_gain_count = (uint32_t)rtlsdr_get_tuner_gains(NULL);
                g_shared.sdr_ready = true;
                usb_host_set_open(true);
                printf("[rtl] ready: tuner %s, %lu gain steps\n", rtlsdr_tuner_name(),
                       (unsigned long)g_shared.tuner_gain_count);
            } else {
                printf("[rtl] initialisation FAILED (unsupported tuner or USB error)\n");
                g_shared.tuner_type = 0;
                g_shared.tuner_gain_count = 0;
                g_shared.sdr_ready = false;
                usb_host_set_open(true);   /* still allow raw streaming for diagnostics */
            }
            if (g_shared.client_connected) {
                rtlsdr_reset_buffer();
                usb_stream_reset_toggle();
                usb_stream_start();
                streaming = true;
            }
        }
        /* the bulk pipe gave up (STALL / repeated errors): reset the dongle FIFO and restart */
        if (streaming && usb_stream_fault() && usb_host_state() == USBH_DEVICE_OPEN) {
            static absolute_time_t restart_at;
            static bool restart_armed = false;
            if (!restart_armed) { restart_at = make_timeout_time_ms(500); restart_armed = true; }
            else if (time_reached(restart_at)) {
                restart_armed = false;
                printf("[usb] restarting stream after fault\n");
                usb_stream_stop();
                rtlsdr_reset_buffer();
                usb_stream_reset_toggle();
                ring_reset(&g_shared.iq_ring);
                usb_stream_start();
            }
        }
        if (usb_host_state() == USBH_NO_DEVICE) {
            g_shared.sdr_ready = false;
            streaming = false;
        }

        /* commands from the TCP client (core1) */
        rtltcp_cmd_t c;
        if (queue_try_remove(&g_shared.cmd_queue, &c)) {
            if (c.cmd == RTLTCP_EVT_CLIENT_CONNECTED) {
                if (usb_host_state() == USBH_DEVICE_OPEN) {
                    usb_stream_stop();
                    rtlsdr_reset_buffer();
                    usb_stream_reset_toggle();
                    ring_reset(&g_shared.iq_ring);   /* core0 is the only writer of head */
                    usb_stream_start();
                    streaming = true;
                }
            } else if (c.cmd == RTLTCP_EVT_CLIENT_DISCONNECTED) {
                usb_stream_stop();
                streaming = false;
                ring_reset(&g_shared.iq_ring);
            } else if (usb_host_state() == USBH_DEVICE_OPEN && g_shared.sdr_ready) {
                /* EPX is shared: idle the bulk pipe once for the whole command */
                usb_stream_pause();
                execute_cmd(&c);
                usb_stream_resume();
            }
        }

        /* status LED */
        if (usb_host_state() == USBH_NO_DEVICE) leds_set(LED_NO_SDR);
        else if (usb_host_state() != USBH_DEVICE_OPEN) leds_set(LED_BOOT);
        else if (!g_shared.sdr_ready)                  leds_set(LED_ERROR);
        else if (!g_shared.link_up || !g_shared.ip_ready) leds_set(LED_NO_LINK);
        else if (streaming)                            leds_set(LED_STREAMING);
        else                                           leds_set(LED_IDLE);

        if (time_reached(next_stats)) {
            next_stats = make_timeout_time_ms(CFG_STATUS_PRINT_SEC * 1000);
            print_stats();
        }
    }
    return 0;
}
