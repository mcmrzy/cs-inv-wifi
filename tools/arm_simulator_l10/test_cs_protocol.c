/*
 * test_cs_protocol.c — C 实现 (cs_protocol.c) 交叉验证宿主测试
 *
 * 编译: cl.exe /W3 /I<esp32c3_l10_idf/main> test_cs_protocol.c
 * 运行: 逐向量比较 cs_frame_send() 输出与 Python 参考向量,
 *       并验证 发送→接收 往返一致性。
 */
#include <stdio.h>
#include <string.h>
#include "cs_protocol.h"
#include "test_vectors.h"

static int g_fail = 0;
static int g_pass = 0;

static void check(int cond, const char *msg)
{
    if (cond) {
        g_pass++;
    } else {
        g_fail++;
        printf("  [FAIL] %s\n", msg);
    }
}

int main(void)
{
    size_t i;
    printf("cs_protocol cross-validation (%u vectors)\n", (unsigned)VECTOR_COUNT);

    for (i = 0; i < VECTOR_COUNT; i++) {
        const test_vector_t *v = &g_vectors[i];
        uint8_t out[512];
        size_t n = cs_frame_send(out, v->cmd, v->param1, v->param2,
                                 v->data_len ? v->data : NULL, v->rand);
        char buf[64];

        if (n != v->expect_len) {
            g_fail++;
            printf("  [FAIL] %s: len %u != expect %u\n", v->name,
                   (unsigned)n, (unsigned)v->expect_len);
            continue;
        }
        if (memcmp(out, v->expect, n) != 0) {
            size_t k;
            g_fail++;
            printf("  [FAIL] %s: bytes mismatch\n", v->name);
            printf("    got     : ");
            for (k = 0; k < n; k++) printf("%02x", out[k]);
            printf("\n    expect  : ");
            for (k = 0; k < n; k++) printf("%02x", v->expect[k]);
            printf("\n");
            continue;
        }
        snprintf(buf, sizeof(buf), "%s: ppp bytes match", v->name);
        check(1, buf);
        printf("  [PASS] %s\n", v->name);
    }

    /* 往返测试: C 发送 → C PPP 接收 → 解密 → 解析 */
    printf("\nround-trip (C send -> C ppp rx -> decrypt -> parse)\n");
    for (i = 0; i < VECTOR_COUNT; i++) {
        const test_vector_t *v = &g_vectors[i];
        uint8_t ppp[512];
        size_t n = cs_frame_send(ppp, v->cmd, v->param1, v->param2,
                                 v->data_len ? v->data : NULL, v->rand);
        cs_ppp_rx_t rx;
        size_t k;
        uint8_t raw[512];
        int complete = 0;
        uint8_t cmd;
        uint16_t p1, p2;
        const uint8_t *data;
        char buf[64];

        cs_ppp_rx_init(&rx);
        for (k = 0; k < n; k++) {
            if (cs_ppp_rx_byte(&rx, ppp[k])) {
                complete = 1;
                break;
            }
        }
        snprintf(buf, sizeof(buf), "%s: ppp frame completed", v->name);
        check(complete, buf);
        if (!complete) continue;

        memcpy(raw, rx.buf, rx.length);
        snprintf(buf, sizeof(buf), "%s: decrypt ok", v->name);
        check(cs_decrypt_rx(raw, rx.length) == 0, buf);
        snprintf(buf, sizeof(buf), "%s: parse ok", v->name);
        check(cs_frame_parse(raw, rx.length, &cmd, &p1, &p2, &data) == 0, buf);

        if (cmd != v->cmd || p1 != v->param1 || p2 != v->param2 ||
            (v->data_len && memcmp(data, v->data, v->data_len) != 0)) {
            g_fail++;
            printf("  [FAIL] %s: round-trip content mismatch\n", v->name);
            continue;
        }
        printf("  [PASS] %s round-trip\n", v->name);
    }

    /* PPP 非法转义丢弃测试 */
    {
        cs_ppp_rx_t rx;
        cs_ppp_rx_init(&rx);
        cs_ppp_rx_byte(&rx, CS_PPP_ESC_7E);
        cs_ppp_rx_byte(&rx, CS_PPP_ESC_7D);
        cs_ppp_rx_byte(&rx, 0x41);
        check(rx.status == CS_PPP_RX_WAIT, "ppp invalid escape resets");
    }
    /* 正常转义: 0x7D 0x5E -> 0x7E */
    {
        cs_ppp_rx_t rx;
        cs_ppp_rx_init(&rx);
        cs_ppp_rx_byte(&rx, CS_PPP_ESC_7E);
        cs_ppp_rx_byte(&rx, CS_PPP_ESC_7D);
        cs_ppp_rx_byte(&rx, CS_PPP_ESC_5E);
        cs_ppp_rx_byte(&rx, CS_PPP_ESC_7E);
        check(rx.status == CS_PPP_RX_FINISH && rx.length == 1 &&
              rx.buf[0] == CS_PPP_ESC_7E, "ppp escape 7E decodes");
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
