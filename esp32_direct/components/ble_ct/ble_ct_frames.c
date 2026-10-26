#include "ble_ct_frames.h"
#include <string.h>

void ble_ct_frames_reset(ble_ct_frame_rx_t *state)
{
    if (state) {
        state->len = 0;
        state->next_seq = 0;
        state->active = false;
        state->updated_ms = 0;
    }
}

int ble_ct_frames_accept(ble_ct_frame_rx_t *state, const uint8_t *frame,
                        size_t len, uint32_t now_ms, uint32_t timeout_ms,
                        size_t message_limit)
{
    if (!state) return BLE_CT_FRAME_ERROR;
    if (!frame || len < 2 || !message_limit ||
        message_limit > BLE_CT_FRAME_MAX_MESSAGE_BYTES) goto invalid;
    const uint8_t ctrl = frame[0];
    const uint8_t seq = ctrl & 0x3fu;
    const bool first = (ctrl & BLE_CT_FRAME_FIRST) != 0;
    const bool last = (ctrl & BLE_CT_FRAME_LAST) != 0;
    if (state->active && (uint32_t)(now_ms - state->updated_ms) > timeout_ms)
        goto invalid;
    if (first) {
        if (seq != 0 || state->active) goto invalid;
        state->len = 0;
        state->next_seq = 0;
    } else if (!state->active) {
        goto invalid;
    }
    if (seq != state->next_seq || (!last && seq == BLE_CT_FRAME_MAX_FRAMES - 1u))
        goto invalid;
    if (state->len > message_limit || len - 1u > message_limit - state->len)
        goto invalid;
    memcpy(state->data + state->len, frame + 1, len - 1u);
    state->len += (uint16_t)(len - 1u);
    state->next_seq = seq + 1u;
    state->updated_ms = now_ms;
    state->active = !last;
    return last ? BLE_CT_FRAME_COMPLETE : BLE_CT_FRAME_MORE;
invalid:
    ble_ct_frames_reset(state);
    return BLE_CT_FRAME_ERROR;
}

size_t ble_ct_frame_payload_capacity(uint16_t mtu)
{
    if (mtu > 512) mtu = 512;
    return mtu > 4 ? mtu - 4u : 0;
}

int ble_ct_frames_next(const uint8_t *message, size_t len, uint16_t mtu,
                      size_t *offset, uint8_t *seq, uint8_t *frame,
                      size_t capacity, size_t *frame_len)
{
    const size_t payload = ble_ct_frame_payload_capacity(mtu);
    if (!message || !len || len > BLE_CT_FRAME_MAX_MESSAGE_BYTES ||
        !offset || !seq || !frame || !frame_len || !payload ||
        *offset >= len || *seq >= BLE_CT_FRAME_MAX_FRAMES ||
        (*offset == 0) != (*seq == 0) ||
        (len + payload - 1u) / payload > BLE_CT_FRAME_MAX_FRAMES)
        return BLE_CT_FRAME_ERROR;
    const size_t remaining = len - *offset;
    const size_t chunk = remaining < payload ? remaining : payload;
    if (capacity < chunk + 1u) return BLE_CT_FRAME_ERROR;
    uint8_t ctrl = *seq;
    if (*offset == 0) ctrl |= BLE_CT_FRAME_FIRST;
    if (chunk == remaining) ctrl |= BLE_CT_FRAME_LAST;
    frame[0] = ctrl;
    memcpy(frame + 1, message + *offset, chunk);
    *frame_len = chunk + 1u;
    *offset += chunk;
    ++*seq;
    return *offset == len ? BLE_CT_FRAME_COMPLETE : BLE_CT_FRAME_MORE;
}
