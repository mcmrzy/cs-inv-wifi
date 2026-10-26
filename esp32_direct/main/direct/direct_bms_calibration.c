#include "direct_bms_calibration.h"
#include "direct_protocol.h"
#include <string.h>

static bool valid_request(const direct_bms_calibration_request_t *r)
{
    if (!r || r->point > 1) return false;
    if (r->command == 0x0a)
        return r->cell_count > 0 && r->cell_count <= 16 &&
               r->reference > 0 && r->reference < 4000;
    if (r->command == 0x0b || r->command == 0x0c)
        return r->cell_count == 0 && r->reference <= 65535;
    return false;
}

size_t direct_bms_calibration_make_request(
    const direct_bms_calibration_request_t *r, uint8_t confirmed_cells,
    uint8_t *out, size_t capacity)
{
    if (!valid_request(r) || !out) return 0;
    if (r->command == 0x0a &&
        (confirmed_cells == 0 || confirmed_cells > 16 ||
         r->cell_count > confirmed_cells)) return 0;
    const size_t length = r->command == 0x0a ? 11 : 10;
    if (capacity < length) return 0;
    memset(out, 0, length);
    out[0] = 0x7c; out[3] = 0x80; out[4] = r->command;
    out[5] = (uint8_t)(length - 7); out[7] = r->point;
    size_t value_offset = 8;
    if (r->command == 0x0a) out[value_offset++] = r->cell_count;
    out[value_offset] = (uint8_t)r->reference;
    out[value_offset + 1] = (uint8_t)(r->reference >> 8);
    const uint16_t crc = direct_crc16(out + 3, length - 3);
    out[1] = (uint8_t)crc; out[2] = (uint8_t)(crc >> 8);
    return length;
}

bool direct_bms_calibration_parse_ack(
    const uint8_t *frame, size_t length,
    const direct_bms_calibration_request_t *r, bool *stored)
{
    if (!frame || !stored || !valid_request(r)) return false;
    const size_t expected_length = r->command == 0x0a ? 12 : 11;
    if (length != expected_length || frame[0] != 0x7c || frame[3] != 0x80 ||
        frame[4] != r->command || frame[5] != expected_length - 7 ||
        frame[6] != 0 || frame[7] != r->point ||
        direct_crc16(frame + 3, length - 3) !=
            (uint16_t)(frame[1] | ((uint16_t)frame[2] << 8))) return false;
    size_t value_offset = 8;
    if (r->command == 0x0a && frame[value_offset++] != r->cell_count) return false;
    const uint16_t value = (uint16_t)(frame[value_offset] |
                                    ((uint16_t)frame[value_offset + 1] << 8));
    if (value != r->reference || frame[length - 1] > 1) return false;
    *stored = frame[length - 1] == 1;
    return true;
}
