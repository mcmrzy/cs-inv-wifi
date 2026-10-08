#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arm_iap_codec.h"
#include "cs_protocol.h"

static int failures;
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    failures++; \
} } while (0)

static size_t unpack_ppp(const uint8_t *frame, size_t frame_len,
                         uint8_t *raw, size_t capacity)
{
    if (frame_len < 2 || frame[0] != CS_PPP_ESC_7E ||
        frame[frame_len - 1] != CS_PPP_ESC_7E) return 0;
    size_t out = 0;
    for (size_t i = 1; i + 1 < frame_len; ++i) {
        uint8_t value = frame[i];
        if (value == CS_PPP_ESC_7D) {
            if (++i + 1 >= frame_len) return 0;
            if (frame[i] == CS_PPP_ESC_5E) value = CS_PPP_ESC_7E;
            else if (frame[i] == CS_PPP_ESC_5D) value = CS_PPP_ESC_7D;
            else return 0;
        }
        if (out >= capacity) return 0;
        raw[out++] = value;
    }
    return out;
}

static size_t decode_cs_frame(const uint8_t *frame, size_t frame_len,
                              uint8_t *raw)
{
    cs_ppp_rx_t rx;
    cs_ppp_rx_init(&rx);
    int complete = 0;
    for (size_t i = 0; i < frame_len; ++i) {
        complete = cs_ppp_rx_byte(&rx, frame[i]);
    }
    CHECK(complete);
    memcpy(raw, rx.buf, rx.length);
    CHECK(cs_decrypt_rx(raw, rx.length) == 0);
    return rx.length;
}

static void test_bms_summary(void)
{
    uint8_t frame[512], raw[CS_FRAME_MAX], payload[CS_BATTSUM_DATA_SIZE + 4];
    uint8_t cmd = 0;
    uint16_t param1 = 1, param2 = 1;
    const uint8_t *decoded = NULL;
    for (size_t i = 0; i < sizeof(payload); ++i) {
        payload[i] = (uint8_t)(i * 29u + 0x7du);
    }

    size_t frame_len = cs_frame_send(frame, CS_CMD_READ_BATTSUM, 0, 0,
                                    NULL, 0x12345678u);
    size_t raw_len = decode_cs_frame(frame, frame_len, raw);
    CHECK(raw_len == 24);
    CHECK(raw[CS_OFF_CMDLEN] == 8 && raw[CS_OFF_CMDLEN + 1] == 0);
    CHECK(cs_frame_parse(raw, raw_len, &cmd, &param1, &param2, &decoded) == 0);
    CHECK(cmd == CS_CMD_READ_BATTSUM && param1 == 0 && param2 == 0);
    CHECK(cs_frame_parse_bms_summary(raw, raw_len, &decoded) != 0);

    frame_len = cs_frame_send(frame, CS_CMD_READ_BATTSUM, 0,
                             CS_BATTSUM_DATA_SIZE, payload, 0xabcdef01u);
    raw_len = decode_cs_frame(frame, frame_len, raw);
    CHECK(raw_len == 120);
    CHECK(raw[CS_OFF_CMDLEN] == 108 && raw[CS_OFF_CMDLEN + 1] == 0);
    CHECK(cs_frame_parse_bms_summary(raw, raw_len, &decoded) == 0);
    CHECK(memcmp(decoded, payload, CS_BATTSUM_DATA_SIZE) == 0);
    CHECK(cs_frame_parse_bms_summary(raw, raw_len - 1, &decoded) != 0);
    CHECK(cs_frame_parse_bms_summary(raw, raw_len + 1, &decoded) != 0);

    raw[CS_OFF_DATA + 3] ^= 0x80;
    CHECK(cs_frame_parse_bms_summary(raw, raw_len, &decoded) == -2);

    const uint16_t wrong_sizes[] = {0, 96, 99, 101, 104};
    for (size_t i = 0; i < sizeof(wrong_sizes) / sizeof(wrong_sizes[0]); ++i) {
        frame_len = cs_frame_send(frame, CS_CMD_READ_BATTSUM, 0,
                                 wrong_sizes[i], payload, (uint32_t)i);
        raw_len = decode_cs_frame(frame, frame_len, raw);
        /* These have valid crypto/CRC, so rejection must enforce the layout. */
        CHECK(cs_frame_parse(raw, raw_len, NULL, NULL, NULL, NULL) == 0);
        CHECK(cs_frame_parse_bms_summary(raw, raw_len, &decoded) != 0);
    }
    frame_len = cs_frame_send(frame, CS_CMD_READ_BATTSUM, 1,
                             CS_BATTSUM_DATA_SIZE, payload, 0x7d7e7d7eu);
    raw_len = decode_cs_frame(frame, frame_len, raw);
    CHECK(cs_frame_parse(raw, raw_len, NULL, NULL, NULL, NULL) == 0);
    CHECK(cs_frame_parse_bms_summary(raw, raw_len, &decoded) != 0);

    frame_len = cs_frame_send(frame, CS_CMD_READ_RUNPARAM, 0,
                             CS_BATTSUM_DATA_SIZE, payload, 0u);
    raw_len = decode_cs_frame(frame, frame_len, raw);
    CHECK(cs_frame_parse_bms_summary(raw, raw_len, &decoded) != 0);

    /* A short CRC-valid CmdLen must not borrow its missing payload from the key tail. */
    frame_len = cs_frame_send(frame, CS_CMD_READ_BATTSUM, 0,
                             CS_BATTSUM_DATA_SIZE, payload, 42u);
    raw_len = decode_cs_frame(frame, frame_len, raw);
    raw[CS_OFF_CMDLEN] = 104;
    uint32_t crc = cs_crc32_words(raw, 104 / 4);
    for (int i = 0; i < 4; ++i) raw[104 + i] = (uint8_t)(crc >> (8 * i));
    CHECK(cs_frame_parse(raw, raw_len, NULL, NULL, NULL, NULL) == 0);
    CHECK(cs_frame_parse_bms_summary(raw, raw_len, &decoded) != 0);
}

int main(void)
{
    enum { PAYLOAD_SIZE = 2 + ARM_IAP_FIRST_PAYLOAD_SIZE };
    uint8_t payload[PAYLOAD_SIZE];
    uint8_t frame[CS_FRAME_TX_MAX * 2 + 2];
    uint8_t raw[CS_FRAME_TX_MAX];
    for (size_t i = 0; i < sizeof(payload); ++i)
        payload[i] = (uint8_t)(i * 29u + 0x7du);

    const size_t frame_len = cs_frame_send(
        frame, 0x0B, 2, sizeof(payload), payload, 0x12345678u);
    CHECK(frame_len > 2 && frame_len <= sizeof(frame));
    const size_t raw_len = unpack_ppp(frame, frame_len, raw, sizeof(raw));
    CHECK(raw_len > 0);
    CHECK(cs_decrypt_rx(raw, raw_len) == 0);

    uint8_t cmd = 0;
    uint16_t param1 = 0, param2 = 0;
    const uint8_t *decoded = NULL;
    CHECK(cs_frame_parse(raw, raw_len, &cmd, &param1, &param2, &decoded) == 0);
    CHECK(cmd == 0x0B);
    CHECK(param1 == 2);
    CHECK(param2 == sizeof(payload));
    CHECK(decoded != NULL && memcmp(decoded, payload, sizeof(payload)) == 0);
    test_bms_summary();
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
