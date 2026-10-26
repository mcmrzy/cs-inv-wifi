#pragma once
#include <stdint.h>

enum {
    DIRECT_KEY_NONE = 0, DIRECT_KEY_BACK = 1, DIRECT_KEY_ENTER = 2,
    DIRECT_KEY_UP = 3, DIRECT_KEY_LEFT = 4, DIRECT_KEY_DOWN = 5,
    DIRECT_KEY_RIGHT = 6, DIRECT_KEY_ENTER_BACK = 7,
    DIRECT_KEY_NO_FRAME = -1, DIRECT_KEY_BAD_FRAME = -2,
};

typedef struct {
    uint8_t held;
    uint32_t last_frame_ms;
    uint32_t last_event_ms;
    /* Set only when a physical key is newly present in a valid touch frame. */
    uint8_t press_edge;
} direct_key_state_t;

/* A valid zero-touch frame returns NONE; corrupt/absent frames are distinct. */
int direct_key_decode(uint16_t frame);
/* Confirm/back are edges; directions repeat at 200ms. Only valid frames release.
 * Unsigned timestamps support timer wrap. NO_FRAME/BAD_FRAME never rearm a key. */
uint8_t direct_key_update(direct_key_state_t *state, int sample, uint32_t now_ms);
/* ARM MarkRead pauses the hardware clock for 4 x 50ms after a touch frame. */
uint32_t direct_key_scan_interval(int sample);
