/* WS2812 status LED (GPIO25 on RP2350-POE-ETH) */
#ifndef LEDS_H
#define LEDS_H

typedef enum {
    LED_BOOT = 0,      /* white   : booting */
    LED_NO_SDR,        /* red     : no RTL-SDR on USB */
    LED_NO_LINK,       /* yellow  : SDR ok, Ethernet link down / no IP */
    LED_IDLE,          /* blue    : ready, waiting for rtl_tcp client */
    LED_STREAMING,     /* green   : client connected, streaming */
    LED_ERROR,         /* magenta : error (tuner init failed etc.) */
    LED_SCAN_HIT,      /* cyan    : fast scanner is receiving a channel */
} led_state_t;

void leds_init(void);
void leds_set(led_state_t s);

#endif
