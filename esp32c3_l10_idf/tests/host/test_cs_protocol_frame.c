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
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
