#include "led1_waveform.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    led1_pulse_t pulses[LED1_SYMBOLS];
    led1_build_waveform(pulses);
    unsigned total_ticks = 0;
    for (unsigned byte = 0; byte < 9; ++byte) {
        unsigned decoded = 0;
        for (unsigned bit = 0; bit < 8; ++bit) {
            const led1_pulse_t pulse = pulses[byte * 8 + bit];
            assert(pulse.high + pulse.low == 25); /* 800kbit/s at 20MHz. */
            assert((pulse.high == 7 && pulse.low == 18) ||
                   (pulse.high == 14 && pulse.low == 11));
            decoded = (decoded << 1) | (pulse.high > pulse.low);
            total_ticks += pulse.high + pulse.low;
        }
        assert(decoded == (byte == 1 ? 64 : 0)); /* GRB: LED1 red, others off. */
    }
    assert(total_ticks == 1800); /* 90us data + 300us low latch. */
    assert(pulses[LED1_DATA_BITS].high + pulses[LED1_DATA_BITS].low == 6000);
    assert(LED1_SYMBOLS + 1 <= 96); /* Includes RMT EOF. */
    puts("PASS LED1 wire decode: GRB 00 40 00 / 00 00 00 / 00 00 00, 800kHz, 300us latch, fits RMT memory");
    return 0;
}
