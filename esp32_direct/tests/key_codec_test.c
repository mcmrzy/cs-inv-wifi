#include <assert.h>
#include <stdio.h>
#include "direct_key_codec.h"

int main(void)
{
    for(int key=1;key<=7;key++)assert(direct_key_scan_interval(key)==200);
    assert(direct_key_scan_interval(DIRECT_KEY_NO_FRAME)==20);
    assert(direct_key_scan_interval(DIRECT_KEY_BAD_FRAME)==20);
    assert(direct_key_scan_interval(DIRECT_KEY_NONE)==20);
    const uint8_t values[] = {0xef, 0xfe, 0xdf, 0xf7, 0xfb, 0xfd, 0xee};
    for (unsigned i = 0; i < 7; ++i)
        assert(direct_key_decode(0xa100U | values[i]) == (int)i + 1);
    assert(direct_key_decode(0x9100) == DIRECT_KEY_BAD_FRAME);
    assert(direct_key_decode(0xa0ef) == DIRECT_KEY_NONE);
    assert(direct_key_decode(0xa1fc) == DIRECT_KEY_BAD_FRAME);
    assert(direct_key_decode(0xa1ff) == DIRECT_KEY_NONE);
    direct_key_state_t s = {0};
    assert(direct_key_update(&s, 2, 0) == 2);
    assert(direct_key_update(&s, DIRECT_KEY_NO_FRAME, 10) == 0);
    assert(direct_key_update(&s, DIRECT_KEY_BAD_FRAME, 20) == 0);
    assert(direct_key_update(&s, 2, 30) == 0);
    assert(direct_key_update(&s, 2, 199) == 0);
    assert(direct_key_update(&s, 2, 200) == 0);
    assert(direct_key_update(&s, 0, 210) == 0);
    assert(direct_key_update(&s, 2, 220) == 2);
    assert(direct_key_update(&s, DIRECT_KEY_NO_FRAME, 519) == 0 && s.held == 2);
    assert(direct_key_update(&s, DIRECT_KEY_NO_FRAME, 520) == 0 && s.held == 2);
    assert(direct_key_update(&s, DIRECT_KEY_BAD_FRAME, 900) == 0 && s.held == 2);
    assert(direct_key_update(&s, 2, 1000) == 0);
    assert(direct_key_update(&s, 7, 600) == 7);
    for (unsigned i = 1; i <= 10; ++i)
        assert(direct_key_update(&s, 7, 600 + 200 * i) == 0);
    s = (direct_key_state_t){0};
    assert(direct_key_update(&s, 3, UINT32_MAX - 100U) == 3);
    assert(s.press_edge);
    assert(direct_key_update(&s, 3, 98) == 0);
    assert(direct_key_update(&s, 3, 99) == 3);
    assert(!s.press_edge);

    /* Enter/back can wake the screen or trigger saving once, but remaining
     * held across either page change must never enter/confirm the next page. */
    for (int key = DIRECT_KEY_BACK; key <= DIRECT_KEY_ENTER; ++key) {
        s = (direct_key_state_t){0};
        assert(direct_key_update(&s, key, 100) == key && s.press_edge);
        for (uint32_t t = 300; t <= 2100; t += 200)
            assert(direct_key_update(&s, key, t) == 0 && !s.press_edge);
        assert(direct_key_update(&s, DIRECT_KEY_NO_FRAME, 3000) == 0);
        assert(direct_key_update(&s, DIRECT_KEY_BAD_FRAME, 10000) == 0);
        assert(direct_key_update(&s, key, 10200) == 0 && !s.press_edge);
        assert(direct_key_update(&s, DIRECT_KEY_NONE, 10220) == 0);
        assert(direct_key_update(&s, key, 10240) == key && s.press_edge);
    }
    for (int key = DIRECT_KEY_UP; key <= DIRECT_KEY_RIGHT; ++key) {
        s = (direct_key_state_t){0};
        assert(direct_key_update(&s, key, 100) == key && s.press_edge);
        assert(direct_key_update(&s, key, 299) == 0 && !s.press_edge);
        assert(direct_key_update(&s, key, 300) == key && !s.press_edge);
        assert(direct_key_update(&s, DIRECT_KEY_BAD_FRAME, 700) == 0);
        assert(direct_key_update(&s, key, 900) == key && !s.press_edge);
        assert(direct_key_update(&s, DIRECT_KEY_NONE, 920) == 0);
        assert(direct_key_update(&s, key, 940) == key && s.press_edge);
    }
    /* Neither half of a released chord is a fresh confirmation/back press. */
    for (int key = DIRECT_KEY_BACK; key <= DIRECT_KEY_ENTER; ++key) {
        s = (direct_key_state_t){0};
        assert(direct_key_update(&s, key, 0) == key);
        assert(direct_key_update(&s, DIRECT_KEY_ENTER_BACK, 200) == DIRECT_KEY_ENTER_BACK);
        assert(direct_key_update(&s, key, 400) == 0 && !s.press_edge);
        assert(direct_key_update(&s, key, 600) == 0 && !s.press_edge);
        assert(direct_key_update(&s, DIRECT_KEY_NONE, 620) == 0);
        assert(direct_key_update(&s, key, 640) == key && s.press_edge);
    }
    s = (direct_key_state_t){0};
    assert(direct_key_update(&s, DIRECT_KEY_ENTER, UINT32_MAX - 100U) == DIRECT_KEY_ENTER);
    assert(direct_key_update(&s, DIRECT_KEY_NO_FRAME, 500) == 0);
    assert(direct_key_update(&s, DIRECT_KEY_ENTER, 700) == 0);
    assert(direct_key_update(&s, DIRECT_KEY_NONE, 720) == 0);
    assert(direct_key_update(&s, DIRECT_KEY_ENTER, 740) == DIRECT_KEY_ENTER);
    assert(direct_key_update(&s, 99, 1000) == 0 && s.held == DIRECT_KEY_ENTER);
    puts("PASS: BS818A decode, valid-release edges, direction-only repeat, press classification, chord and timestamp wrap");
    return 0;
}
