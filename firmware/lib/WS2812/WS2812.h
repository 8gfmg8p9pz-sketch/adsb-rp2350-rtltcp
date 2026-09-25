#ifndef WS2812_H
#define WS2812_H

#include <stdio.h>
#include <stdlib.h>
#include "pico/stdlib.h"


#define NUM_LEDS 1
#define LED_BRIGHTNESS 50
#define WIDTH 1
#define HEIGHT 1
#define WS2812_LED_IN   (25)

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} RGB;

typedef struct {
    uint16_t num_leds;    
    uint8_t brightness;    
    uint8_t width;         
    uint8_t height;        
    RGB leds[NUM_LEDS];
} LEDController;

extern LEDController led_ctrl;

void WS2812_init();
void WS2812_set_pixel(uint8_t x, uint8_t y, uint8_t r, uint8_t g, uint8_t b);
void WS2812_clear();
void WS2812_show();
void WS2812_show2(uint8_t r, uint8_t g, uint8_t b);

#endif // WS2812_H
