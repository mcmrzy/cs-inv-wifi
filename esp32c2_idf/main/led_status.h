#ifndef LED_STATUS_H
#define LED_STATUS_H

#include "config.h"

typedef enum {
    LED_OFF = 0,
    LED_SLOW_BLINK,
    LED_FAST_BLINK,
    LED_SOLID,
    LED_FAULT,
    LED_BREATH,
    LED_3FLASH,
} LedPattern;

void led_init();
void led1_set(LedPattern state);
void led2_set(LedPattern state);
void led_signal_data();
void led_tick();

#endif
