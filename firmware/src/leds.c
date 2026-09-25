#include "leds.h"
#include "WS2812.h"

static led_state_t s_cur = (led_state_t)-1;

void leds_init(void)
{
    WS2812_init();
    leds_set(LED_BOOT);
}

void leds_set(led_state_t s)
{
    if (s == s_cur) return;
    s_cur = s;
    switch (s) {
    case LED_BOOT:      WS2812_show2(40, 40, 40); break;
    case LED_NO_SDR:    WS2812_show2(60, 0, 0);   break;
    case LED_NO_LINK:   WS2812_show2(50, 40, 0);  break;
    case LED_IDLE:      WS2812_show2(0, 0, 60);   break;
    case LED_STREAMING: WS2812_show2(0, 60, 0);   break;
    case LED_ERROR:     WS2812_show2(60, 0, 60);  break;
    default: break;
    }
}
