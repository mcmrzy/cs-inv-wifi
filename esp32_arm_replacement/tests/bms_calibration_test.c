#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "direct_bms_calibration.h"
#include "direct_protocol.h"

static void seal(uint8_t *frame, size_t length)
{
    const uint16_t crc = direct_crc16(frame + 3, length - 3);
    frame[1] = (uint8_t)crc; frame[2] = (uint8_t)(crc >> 8);
}

int main(void)
{
    uint8_t tx[11], ack[12];
    bool stored = false;
    direct_bms_calibration_request_t r = {0x0a, 0, 16, 3500};
    assert(direct_bms_calibration_make_request(&r, 16, tx, sizeof(tx)) == 11);
    assert(tx[0] == 0x7c && tx[3] == 0x80 && tx[4] == 0x0a);
    assert(tx[5] == 4 && tx[6] == 0 && tx[7] == 0 && tx[8] == 16);
    assert(tx[9] == 0xac && tx[10] == 0x0d);
    memcpy(ack, tx, 11); ack[5] = 5; ack[11] = 1; seal(ack, 12);
    assert(direct_bms_calibration_parse_ack(ack, 12, &r, &stored) && stored);
    ack[11] = 0; seal(ack, 12);
    assert(direct_bms_calibration_parse_ack(ack, 12, &r, &stored) && !stored);
    stored = true; ack[11] = 2; seal(ack, 12);
    assert(!direct_bms_calibration_parse_ack(ack, 12, &r, &stored) && stored);
    ack[11] = 1; ack[9] ^= 1; seal(ack, 12);
    assert(!direct_bms_calibration_parse_ack(ack, 12, &r, &stored));
    r.reference = 4000;
    assert(direct_bms_calibration_make_request(&r, 16, tx, 11) == 0);
    r.reference = 0;
    assert(direct_bms_calibration_make_request(&r, 16, tx, 11) == 0);
    r.reference = 3999; r.cell_count = 17;
    assert(direct_bms_calibration_make_request(&r, 16, tx, 11) == 0);
    r.cell_count = 0;
    assert(direct_bms_calibration_make_request(&r, 16, tx, 11) == 0);
    r.cell_count = 16;
    assert(direct_bms_calibration_make_request(&r, 0, tx, 11) == 0);
    assert(direct_bms_calibration_make_request(&r, 15, tx, 11) == 0);
    assert(direct_bms_calibration_make_request(&r, 17, tx, 11) == 0);
    r.point = 1;
    assert(direct_bms_calibration_make_request(&r, 16, tx, 11) == 11);
    r.point = 2;
    assert(direct_bms_calibration_make_request(&r, 16, tx, 11) == 0);

    for (uint8_t command = 0x0b; command <= 0x0c; ++command) {
        r.command = command; r.point = 1; r.cell_count = 0; r.reference = 250;
        assert(direct_bms_calibration_make_request(&r, 0, tx, 11) == 10);
        assert(tx[5] == 3 && tx[7] == 1 && tx[8] == 250 && tx[9] == 0);
        memcpy(ack, tx, 10); ack[5] = 4; ack[10] = 1; seal(ack, 11);
        assert(direct_bms_calibration_parse_ack(ack, 11, &r, &stored) && stored);
        ack[2] ^= 1;
        assert(!direct_bms_calibration_parse_ack(ack, 11, &r, &stored));
        ack[2] ^= 1; ack[3] = 0x81; seal(ack, 11);
        assert(!direct_bms_calibration_parse_ack(ack, 11, &r, &stored));
        r.reference = 65535;
        assert(direct_bms_calibration_make_request(&r, 0, tx, 11) == 10);
        r.reference = 65536;
        assert(direct_bms_calibration_make_request(&r, 0, tx, 11) == 0);
    }
    r.command = 0x0d; r.reference = 250;
    assert(direct_bms_calibration_make_request(&r, 16, tx, 11) == 0);
    assert(!direct_bms_calibration_parse_ack(ack, 11, &r, &stored));
    r.command = 0x0b;
    assert(direct_bms_calibration_make_request(&r, 0, tx, 9) == 0);
    assert(direct_bms_calibration_make_request(NULL, 0, tx, 11) == 0);
    assert(!direct_bms_calibration_parse_ack(NULL, 11, &r, &stored));
    puts("PASS BMS calibration request range, units, strict acknowledgement");
    return 0;
}
