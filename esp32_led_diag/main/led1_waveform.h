#pragma once
#include <stdint.h>

enum {
    LED1_GPIO = 10,
    LED1_RMT_HZ = 20000000,
    LED1_DATA_BITS = 3 * 24,
    LED1_SYMBOLS = LED1_DATA_BITS + 1,
    LED1_BRIGHTNESS = 64,
};

typedef struct { uint16_t high, low; } led1_pulse_t;

static inline void led1_build_waveform(led1_pulse_t out[LED1_SYMBOLS])
{
    /* GRB: only the first red channel is on. Clear the other two real LEDs. */
    const uint8_t grb[9] = {0, LED1_BRIGHTNESS, 0, 0, 0, 0, 0, 0, 0};
    for (unsigned bit = 0; bit < LED1_DATA_BITS; ++bit) {
        const int one = grb[bit / 8] & (0x80U >> (bit % 8));
        out[bit] = one ? (led1_pulse_t){14, 11} : (led1_pulse_t){7, 18};
    }
    out[LED1_DATA_BITS] = (led1_pulse_t){3000, 3000};
}
