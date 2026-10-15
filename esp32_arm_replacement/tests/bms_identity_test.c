#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "direct_bms_identity.h"
#include "direct_protocol.h"

static void seal(uint8_t frame[32])
{
    uint16_t crc = direct_crc16(frame + 3, 29);
    frame[1] = (uint8_t)crc;
    frame[2] = (uint8_t)(crc >> 8);
}

static void reply(uint8_t frame[32], uint8_t type, const char *text)
{
    memset(frame, 0, 32);
    frame[0] = 0x7c; frame[3] = 0x80; frame[4] = 8;
    frame[5] = 25; frame[7] = type;
    memcpy(frame + 8, text, strlen(text));
    seal(frame);
}

int main(void)
{
    uint8_t query[8], frame[32];
    direct_bms_identity_t identity = {0};
    assert(direct_bms_identity_make_query(0, query) == 8);
    const uint8_t expected[] = {0x7c, 0x76, 0x7e, 0x80, 8, 1, 0, 0};
    assert(memcmp(query, expected, 8) == 0);
    assert(direct_bms_identity_make_query(8, query) == 0);
    assert(direct_bms_identity_make_query(0, NULL) == 0);

    reply(frame, 0, "BOOT.001.1.1");
    assert(direct_bms_identity_parse_reply(frame, 32, 0, &identity));
    assert(strcmp(identity.software, "BOOT.001.1.1") == 0);
    assert(identity.valid_mask == 1);
    reply(frame, 3, "BM.016A.07_20230227");
    assert(direct_bms_identity_parse_reply(frame, 32, 3, &identity));
    assert(strcmp(identity.hardware, "BM.016A.07_20230227") == 0);
    assert(strcmp(identity.software, "BOOT.001.1.1") == 0);
    reply(frame, 4, "AB12-ZYX987654321");
    assert(direct_bms_identity_parse_reply(frame, 32, 4, &identity));
    assert(strcmp(identity.module_sn, "AB12-ZYX987654321") == 0);
    reply(frame, 6, "MODEL_NOT_INVERTER");
    assert(direct_bms_identity_parse_reply(frame, 32, 6, &identity));
    assert(strcmp(identity.product, "MODEL_NOT_INVERTER") == 0);
    reply(frame, 7, "0123ABCD");
    assert(direct_bms_identity_parse_reply(frame, 32, 7, &identity));
    assert(identity.app_crc32 == 0x0123abcdU);
    direct_bms_identity_t before = identity;
    frame[10] ^= 1;
    assert(!direct_bms_identity_parse_reply(frame, 32, 7, &identity));
    assert(memcmp(&identity, &before, sizeof(identity)) == 0);
    seal(frame);
    assert(!direct_bms_identity_parse_reply(frame, 31, 7, &identity));
    assert(!direct_bms_identity_parse_reply(frame, 32, 0, &identity));
    frame[3] = 0x81; seal(frame);
    assert(!direct_bms_identity_parse_reply(frame, 32, 7, &identity));
    reply(frame, 0, "123456789012345678901234");
    assert(direct_bms_identity_parse_reply(frame, 32, 0, &identity));
    assert(strlen(identity.software) == 24);
    reply(frame, 0, "");
    assert(!direct_bms_identity_parse_reply(frame, 32, 0, &identity));
    reply(frame, 0, "FW"); frame[12] = 'X'; seal(frame);
    assert(!direct_bms_identity_parse_reply(frame, 32, 0, &identity));
    reply(frame, 0, "FW"); frame[8] = 0x01; seal(frame);
    assert(!direct_bms_identity_parse_reply(frame, 32, 0, &identity));
    reply(frame, 7, "1234abcd");
    assert(!direct_bms_identity_parse_reply(frame, 32, 7, &identity));
    reply(frame, 7, "1234ABCDZ");
    assert(!direct_bms_identity_parse_reply(frame, 32, 7, &identity));
    reply(frame, 2, "123456789012345678901234");
    assert(direct_bms_identity_parse_reply(frame, 32, 2, &identity));
    reply(frame, 2, "12345678901234567890123Z");
    assert(!direct_bms_identity_parse_reply(frame, 32, 2, &identity));
    reply(frame, 1, "Sep 30 2026,18:30:00");
    assert(direct_bms_identity_parse_reply(frame, 32, 1, &identity));
    reply(frame, 5, "PCB-00001");
    assert(direct_bms_identity_parse_reply(frame, 32, 5, &identity));
    assert(strcmp(identity.pcb_sn, "PCB-00001") == 0);
    frame[4] = 9; seal(frame);
    assert(!direct_bms_identity_parse_reply(frame, 32, 5, &identity));
    frame[4] = 8; frame[6] = 1; seal(frame);
    assert(!direct_bms_identity_parse_reply(frame, 32, 5, &identity));
    puts("PASS BMS identity source wire, strict validation, partial updates");
    return 0;
}
