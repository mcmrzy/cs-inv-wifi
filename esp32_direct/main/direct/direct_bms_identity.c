#include "direct_bms_identity.h"
#include "direct_protocol.h"
#include <string.h>

size_t direct_bms_identity_make_query(uint8_t type, uint8_t out[8])
{
    if (!out || type > 7) return 0;
    const uint8_t fixed[8] = {0x7c, 0, 0, 0x80, 0x08, 1, 0, type};
    memcpy(out, fixed, sizeof(fixed));
    const uint16_t crc = direct_crc16(out + 3, 5);
    out[1] = (uint8_t)crc;
    out[2] = (uint8_t)(crc >> 8);
    return sizeof(fixed);
}

static bool valid_text(const uint8_t *value, size_t *length)
{
    size_t n = 0;
    while (n < 24 && value[n] != 0) {
        if (value[n] < 0x20 || value[n] > 0x7e) return false;
        ++n;
    }
    if (n == 0) return false;
    for (size_t i = n; i < 24; ++i) if (value[i] != 0) return false;
    *length = n;
    return true;
}

bool direct_bms_identity_parse_reply(const uint8_t *frame, size_t len,
                                    uint8_t expected_type,
                                    direct_bms_identity_t *out)
{
    if (!frame || !out || len != 32 || expected_type > 7 ||
        frame[0] != 0x7c || frame[3] != 0x80 || frame[4] != 0x08 ||
        frame[5] != 25 || frame[6] != 0 || frame[7] != expected_type ||
        direct_crc16(frame + 3, 29) !=
            (uint16_t)(frame[1] | ((uint16_t)frame[2] << 8))) return false;
    size_t text_len;
    if (!valid_text(frame + 8, &text_len)) return false;
    if (expected_type == 2) {
        /* STM32F103 CPUID_LEN=12 is emitted as 24 uppercase hex bytes. */
        if (text_len != 24) return false;
        for (size_t i = 8; i < 32; ++i)
            if (!((frame[i] >= '0' && frame[i] <= '9') ||
                  (frame[i] >= 'A' && frame[i] <= 'F'))) return false;
    }
    if (expected_type == 7) {
        if (text_len != 8) return false;
        uint32_t value = 0;
        for (size_t i = 8; i < 16; ++i) {
            uint8_t nibble;
            if (frame[i] >= '0' && frame[i] <= '9') nibble = frame[i] - '0';
            else if (frame[i] >= 'A' && frame[i] <= 'F') nibble = frame[i] - 'A' + 10;
            else return false;
            value = (value << 4) | nibble;
        }
        out->app_crc32 = value;
    } else {
        char *field;
        switch (expected_type) {
            case 0: field = out->software; break;
            case 1: field = out->compile_time; break;
            case 2: field = out->cpuid; break;
            case 3: field = out->hardware; break;
            case 4: field = out->module_sn; break;
            case 5: field = out->pcb_sn; break;
            default: field = out->product; break;
        }
        memcpy(field, frame + 8, 24);
        field[24] = '\0';
    }
    out->valid_mask |= (uint8_t)(1U << expected_type);
    return true;
}
