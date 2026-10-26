#include "direct_key_codec.h"

uint32_t direct_key_scan_interval(int sample)
{
    return sample>=DIRECT_KEY_BACK && sample<=DIRECT_KEY_ENTER_BACK ? 200U : 20U;
}

int direct_key_decode(uint16_t frame)
{
    if ((frame & 0xf000U) != 0xa000U) return DIRECT_KEY_BAD_FRAME;
    if ((frame & 0x0f00U) == 0) return DIRECT_KEY_NONE;
    switch ((uint8_t)frame) {
    case 0xef: return DIRECT_KEY_BACK;
    case 0xfe: return DIRECT_KEY_ENTER;
    case 0xdf: return DIRECT_KEY_UP;
    case 0xf7: return DIRECT_KEY_LEFT;
    case 0xfb: return DIRECT_KEY_DOWN;
    case 0xfd: return DIRECT_KEY_RIGHT;
    case 0xee: return DIRECT_KEY_ENTER_BACK;
    case 0xff: return DIRECT_KEY_NONE;
    default: return DIRECT_KEY_BAD_FRAME;
    }
}

static uint8_t key_mask(uint8_t key)
{
    if (key == DIRECT_KEY_ENTER_BACK)
        return (1U << DIRECT_KEY_ENTER) | (1U << DIRECT_KEY_BACK);
    return key ? (uint8_t)(1U << key) : 0;
}

uint8_t direct_key_update(direct_key_state_t *state, int sample, uint32_t now_ms)
{
    state->press_edge = 0;
    /* The chip may keep DAT high between frames while a finger is still down.
     * Expiring held here would turn a long confirmation into a second press. */
    if (sample < 0 || sample > DIRECT_KEY_ENTER_BACK) return 0;
    state->last_frame_ms = now_ms;
    if (sample == 0) { state->held = 0; return 0; }
    uint8_t previous = state->held;
    state->press_edge = (key_mask((uint8_t)sample) & (uint8_t)~key_mask(previous)) != 0;
    if (previous != sample) {
        state->held = (uint8_t)sample;
        state->last_event_ms = now_ms;
        /* Releasing one finger of ENTER+BACK is not pressing the other again. */
        return state->press_edge ? state->held : 0;
    }
    if (sample >= DIRECT_KEY_UP && sample <= DIRECT_KEY_RIGHT &&
        now_ms - state->last_event_ms >= 200U) {
        state->last_event_ms = now_ms;
        return (uint8_t)sample;
    }
    return 0;
}
