#ifndef BLE_CT_FRAMES_H
#define BLE_CT_FRAMES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BLE_CT_FRAME_FIRST 0x80u
#define BLE_CT_FRAME_LAST 0x40u
#define BLE_CT_FRAME_MAX_MESSAGE_BYTES 1024u
#define BLE_CT_FRAME_MAX_FRAMES 64u

enum { BLE_CT_FRAME_ERROR = -1, BLE_CT_FRAME_MORE = 0, BLE_CT_FRAME_COMPLETE = 1 };

typedef struct {
    uint8_t data[BLE_CT_FRAME_MAX_MESSAGE_BYTES];
    uint16_t len;
    uint8_t next_seq;
    bool active;
    uint32_t updated_ms;
} ble_ct_frame_rx_t;

void ble_ct_frames_reset(ble_ct_frame_rx_t *state);
/* Reject/reset on malformed, duplicate FIRST, gaps, overflow or idle timeout.
 * Completed bytes remain in data/len until reset or the next valid FIRST.
 * Use message_limit=512 for COMMAND/RESULT, 1024 for legacy TELEMETRY. */
int ble_ct_frames_accept(ble_ct_frame_rx_t *state, const uint8_t *frame,
                        size_t len, uint32_t now_ms, uint32_t timeout_ms,
                        size_t message_limit);
/* ATT notification overhead is 3 bytes, frame header is 1. Local MTU <=512. */
size_t ble_ct_frame_payload_capacity(uint16_t mtu);
/* offset/seq start at zero. Writes a complete ATT value; MORE/COMPLETE
 * indicate whether another frame is needed. Invalid input is never truncated. */
int ble_ct_frames_next(const uint8_t *message, size_t len, uint16_t mtu,
                      size_t *offset, uint8_t *seq, uint8_t *frame,
                      size_t capacity, size_t *frame_len);

#endif
