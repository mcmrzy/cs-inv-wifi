#include <assert.h>
#include <stdio.h>
#include "direct_key_codec.h"

int main(void)
{
    /* Schematic KEY1..KEY6: ENTER, RIGHT, DOWN, UP, LEFT, ESC.
     * The serial frame reports these channels as active-low bits 0..5. */
    const int expected[] = {DIRECT_KEY_ENTER, DIRECT_KEY_RIGHT, DIRECT_KEY_DOWN,
                           DIRECT_KEY_UP, DIRECT_KEY_LEFT, DIRECT_KEY_BACK};
    for (unsigned channel = 0; channel < 6; ++channel) {
        uint16_t frame = 0xa100U | (0xffU ^ (1U << channel));
        int decoded = direct_key_decode(frame);
        if (decoded != expected[channel]) {
            fprintf(stderr, "KEY%u frame=%04X: got=%d expected=%d\n",
                    channel + 1, frame, decoded, expected[channel]);
            return 1;
        }
        direct_key_state_t state = {0};
        assert(direct_key_update(&state, decoded, 0) == expected[channel]);
        assert(state.press_edge);
        assert(direct_key_update(&state, decoded, 199) == DIRECT_KEY_NONE);
        assert(direct_key_update(&state, decoded, 200) ==
               (channel == 0 || channel == 5 ? DIRECT_KEY_NONE : expected[channel]));
        assert(direct_key_update(&state, DIRECT_KEY_NONE, 220) == DIRECT_KEY_NONE);
        assert(direct_key_update(&state, decoded, 240) == expected[channel]);
    }
    assert(direct_key_decode(0xa1de) == DIRECT_KEY_ENTER_BACK); /* KEY1+KEY6 */
    assert(direct_key_decode(0xa1ee) == DIRECT_KEY_BAD_FRAME);  /* KEY1+KEY5 */
    assert(direct_key_decode(0xa1ff) == DIRECT_KEY_NONE);
    assert(direct_key_decode(0xa0df) == DIRECT_KEY_NONE);
    assert(direct_key_decode(0x91df) == DIRECT_KEY_BAD_FRAME);
    direct_key_state_t state = {0};
    assert(direct_key_update(&state, direct_key_decode(0xa1de), 0) == DIRECT_KEY_ENTER_BACK);
    assert(direct_key_update(&state, direct_key_decode(0xa1df), 200) == DIRECT_KEY_NONE);
    assert(!state.press_edge); /* Releasing ENTER must not create a BACK press. */
    puts("PASS keys: KEY1 ENTER, KEY2 RIGHT, KEY3 DOWN, KEY4 UP, KEY5 LEFT, KEY6 ESC; repeat and chord");
    return 0;
}
