/*
 * cs_protocol.c — CS-L10-6K2 采集器串口协议 (ESP32 ↔ ARM)
 *
 * 全部逻辑逐字节对齐 ARM 端源码:
 *   Collector.c    帧构造/CRC 位置/双重加密顺序
 *   TEA.c          标准 TEA 32 轮 + CBC 链 (InitV=0)
 *   CRC32.c        逐 32 位字, MSB-first, poly 0x4C11DB7, init 0xFFFFFFFF, 无反射/无最终异或
 *   ComProtocol.c  PPP 0x7E 封装/解封装状态机
 *
 * 注意: 所有多字节字段均为小端序; 本模块不依赖具体平台字节序。
 */
#include "cs_protocol.h"
#include <string.h>

/* FrameKey (TEA.c) */
const uint32_t cs_frame_key[4] = {0x3e23e3fe, 0x4cc197ac, 0x074db01f, 0x42fee1bd};

/* ------------------------------------------------------------------ */
/* 工具: 小端读写                                                  */
/* ------------------------------------------------------------------ */
static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void wr_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/* ------------------------------------------------------------------ */
/* CRC32: 与 CRC32_HW() 一致 (逐 32 位字, MSB-first 逐位, 无最终异或) */
/* ------------------------------------------------------------------ */
uint32_t cs_crc32_words(const uint8_t *buf, size_t word_count)
{
    uint32_t crc = 0xFFFFFFFF;
    size_t i;
    int j;
    for (i = 0; i < word_count; i++) {
        crc ^= rd_le32(buf + i * 4);
        for (j = 0; j < 32; j++) {
            if (crc & 0x80000000u) {
                crc = (crc << 1) ^ 0x4C11DB7u;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

/* ------------------------------------------------------------------ */
/* TEA (标准 32 轮, delta=0x9e3779b9)                                  */
/* ------------------------------------------------------------------ */
void cs_tea_encrypt_block(uint32_t v[2], const uint32_t k[4])
{
    uint32_t v0 = v[0], v1 = v[1];
    uint32_t sum = 0;
    const uint32_t delta = 0x9e3779b9;
    int i;
    for (i = 0; i < 32; i++) {
        sum += delta;
        v0 += ((v1 << 4) + k[0]) ^ (v1 + sum) ^ ((v1 >> 5) + k[1]);
        v1 += ((v0 << 4) + k[2]) ^ (v0 + sum) ^ ((v0 >> 5) + k[3]);
    }
    v[0] = v0;
    v[1] = v1;
}

void cs_tea_decrypt_block(uint32_t v[2], const uint32_t k[4])
{
    uint32_t v0 = v[0], v1 = v[1];
    /* sum = 0xABD47 + 0xC6E479D9 = 0xC6EF3720 (32*delta 同余) */
    uint32_t sum = 0xC6EF3720;
    const uint32_t delta = 0x9e3779b9;
    int i;
    for (i = 0; i < 32; i++) {
        v1 -= ((v0 << 4) + k[2]) ^ (v0 + sum) ^ ((v0 >> 5) + k[3]);
        v0 -= ((v1 << 4) + k[0]) ^ (v1 + sum) ^ ((v1 >> 5) + k[1]);
        sum -= delta;
    }
    v[0] = v0;
    v[1] = v1;
}

/* CBC 加密 (InitV=0, 每块异或前一块密文) — 与 EncryptData() 一致 */
void cs_encrypt_cbc(uint8_t *buf, const uint32_t *key, size_t len)
{
    uint32_t iv[2] = {0, 0};
    size_t i;
    uint32_t v[2];
    for (i = 0; i + 8 <= len; i += 8) {
        v[0] = rd_le32(buf + i) ^ iv[0];
        v[1] = rd_le32(buf + i + 4) ^ iv[1];
        cs_tea_encrypt_block(v, key);
        wr_le32(buf + i, v[0]);
        wr_le32(buf + i + 4, v[1]);
        iv[0] = v[0];
        iv[1] = v[1];
    }
}

/* CBC 解密 — 与 DecryptData() 一致 (先解密再异或前一块原始密文) */
void cs_decrypt_cbc(uint8_t *buf, const uint32_t *key, size_t len)
{
    uint32_t prev[2] = {0, 0};   /* 前一块原始密文 (初始 0) */
    size_t i;
    uint32_t cur[2], v[2];
    for (i = 0; i + 8 <= len; i += 8) {
        cur[0] = rd_le32(buf + i);
        cur[1] = rd_le32(buf + i + 4);
        v[0] = cur[0];
        v[1] = cur[1];
        cs_tea_decrypt_block(v, key);
        wr_le32(buf + i, v[0] ^ prev[0]);
        wr_le32(buf + i + 4, v[1] ^ prev[1]);
        prev[0] = cur[0];
        prev[1] = cur[1];
    }
}

/* ------------------------------------------------------------------ */
/* PPP 封装 (ComProtocol.c PppSend)                                   */
/* ------------------------------------------------------------------ */
size_t cs_ppp_pack_len(size_t len)
{
    return len * 2 + 2;
}

size_t cs_ppp_pack(uint8_t *dst, const uint8_t *src, size_t len)
{
    size_t i, c = 0;
    dst[c++] = CS_PPP_ESC_7E;
    for (i = 0; i < len; i++) {
        if (src[i] == CS_PPP_ESC_7E) {
            dst[c++] = CS_PPP_ESC_7D;
            dst[c++] = CS_PPP_ESC_5E;
        } else if (src[i] == CS_PPP_ESC_7D) {
            dst[c++] = CS_PPP_ESC_7D;
            dst[c++] = CS_PPP_ESC_5D;
        } else {
            dst[c++] = src[i];
        }
    }
    dst[c++] = CS_PPP_ESC_7E;
    return c;
}

/* buf 前 len 字节已是加密原始帧，输出缓冲按 cs_ppp_pack_len(len) 预留。
 * 从尾到头转义，写指针始终位于尚未读取的数据之后，因此无需 4.3KB 常驻
 * scratch，也不会覆盖待处理字节。 */
static size_t cs_ppp_pack_in_place(uint8_t *buf, size_t len)
{
    size_t packed_len = len + 2;
    for (size_t i = 0; i < len; ++i) {
        if (buf[i] == CS_PPP_ESC_7E || buf[i] == CS_PPP_ESC_7D) packed_len++;
    }

    size_t src = len;
    size_t dst = packed_len - 1;
    buf[dst] = CS_PPP_ESC_7E;
    while (src > 0) {
        const uint8_t value = buf[--src];
        if (value == CS_PPP_ESC_7E) {
            buf[--dst] = CS_PPP_ESC_5E;
            buf[--dst] = CS_PPP_ESC_7D;
        } else if (value == CS_PPP_ESC_7D) {
            buf[--dst] = CS_PPP_ESC_5D;
            buf[--dst] = CS_PPP_ESC_7D;
        } else {
            buf[--dst] = value;
        }
    }
    buf[0] = CS_PPP_ESC_7E;
    return packed_len;
}

/* ------------------------------------------------------------------ */
/* PPP 接收状态机 (ComProtocol.c PPPFrameCmdReceive)                  */
/* ------------------------------------------------------------------ */
void cs_ppp_rx_init(cs_ppp_rx_t *rx)
{
    rx->status = CS_PPP_RX_WAIT;
    rx->length = 0;
    rx->flg7e = 0;
}

int cs_ppp_rx_byte(cs_ppp_rx_t *rx, uint8_t b)
{
    switch (rx->status) {
    case CS_PPP_RX_WAIT:
        if (b == CS_PPP_ESC_7E) {
            rx->status = CS_PPP_RX_RCVING;
            rx->length = 0;
            rx->flg7e = 0;
        }
        break;
    case CS_PPP_RX_RCVING:
        if (rx->length > (CS_FRAME_MAX - 1)) {
            /* 超长帧丢弃, 复位后继续处理当前字节 (与 ARM 一致) */
            rx->status = CS_PPP_RX_WAIT;
            rx->length = 0;
            rx->flg7e = 0;
        }
        if (b == CS_PPP_ESC_7E) {
            /* 前一字节为 0x7D 时, 0x7E 为非法转义, 整帧作废 */
            if (rx->flg7e == CS_PPP_ESC_7D) {
                rx->status = CS_PPP_RX_WAIT;
                rx->length = 0;
                rx->flg7e = 0;
                break;
            }
            if (rx->length != 0) {
                rx->status = CS_PPP_RX_FINISH;
            } else {
                rx->status = CS_PPP_RX_RCVING;
            }
            rx->flg7e = 0;
            break;
        } else if (b == CS_PPP_ESC_5E) {
            if (rx->flg7e == CS_PPP_ESC_7D) {
                rx->buf[rx->length++] = CS_PPP_ESC_7E;
                rx->flg7e = 0;
                break;
            }
            rx->buf[rx->length++] = CS_PPP_ESC_5E;
            break;
        } else if (b == CS_PPP_ESC_5D) {
            if (rx->flg7e == CS_PPP_ESC_7D) {
                rx->buf[rx->length++] = CS_PPP_ESC_7D;
                rx->flg7e = 0;
                break;
            }
            rx->buf[rx->length++] = CS_PPP_ESC_5D;
            break;
        } else if (b == CS_PPP_ESC_7D) {
            if (rx->flg7e == CS_PPP_ESC_7D) {
                rx->status = CS_PPP_RX_WAIT;
                rx->length = 0;
                rx->flg7e = 0;
                break;
            }
            rx->flg7e = CS_PPP_ESC_7D;
            break;
        } else {
            /* 前一字节为 0x7D 且当前字节非 5E/5D → 非法转义, 整帧作废 */
            if (rx->flg7e == CS_PPP_ESC_7D) {
                rx->status = CS_PPP_RX_WAIT;
                rx->length = 0;
                rx->flg7e = 0;
                break;
            }
            rx->buf[rx->length++] = b;
            break;
        }
        break;
    default: /* RCVFINISH 或其他 → 复位 (ARM 端由 StartRcv() 完成) */
        rx->status = CS_PPP_RX_WAIT;
        rx->length = 0;
        rx->flg7e = 0;
        break;
    }
    return rx->status == CS_PPP_RX_FINISH;
}

/* ------------------------------------------------------------------ */
/* 发送帧构建: 明文 → CRC → TempKey 加密 → 附 TempKey → FrameKey 加密 */
/* ------------------------------------------------------------------ */
size_t cs_frame_send_len(uint16_t param2)
{
    size_t cmdlen = ((size_t)(param2 + 8 + 3) >> 2) << 2;   /* 4 的倍数 */
    size_t enclen = ((cmdlen + 4 + 7) >> 3) << 3;           /* 8 的倍数 */
    return cs_ppp_pack_len(enclen + 8);
}

size_t cs_frame_send(uint8_t *out, uint8_t cmd, uint16_t param1,
                     uint16_t param2, const uint8_t *data, uint32_t rand)
{
    size_t cmdlen = ((size_t)(param2 + 8 + 3) >> 2) << 2;
    size_t enclen = ((cmdlen + 4 + 7) >> 3) << 3;
    size_t total = enclen + 8;
    uint32_t crc;
    uint32_t tmpk[4];

    if (!out || total > CS_FRAME_TX_MAX) {
        return 0;
    }

    /* 先在 out 头部原地构建/加密，再倒序扩展为 PPP 帧。调用方本来就必须按
     * cs_frame_send_len() 提供最坏 2x 容量，故不再需要静态 raw[]。 */
    memset(out, 0, total);

    /* 帧头 (CollectorCmdDef, pack(1)) */
    out[CS_OFF_HEADER] = CS_HEADER;
    wr_le16(out + CS_OFF_CMDLEN, (uint16_t)cmdlen);
    out[CS_OFF_CMD] = cmd;
    wr_le16(out + CS_OFF_PARAM1, param1);
    wr_le16(out + CS_OFF_PARAM2, param2);
    if (data != NULL && param2 > 0) {
        memcpy(out + CS_OFF_DATA, data, param2);
    }

    /* CRC32 存于帧偏移 CmdLen 处 (帧外 4 字节) */
    crc = cs_crc32_words(out, cmdlen >> 2);
    wr_le32(out + cmdlen, crc);

    /* TempKey = FrameKey[0..1] + {~rand, rand} (ARM: TempKey[3]=SysTick->VAL, [2]=~[3]) */
    tmpk[0] = cs_frame_key[0];
    tmpk[1] = cs_frame_key[1];
    tmpk[2] = ~rand;
    tmpk[3] = rand;

    /* 第一层: TempKey CBC 加密 enclen 字节 */
    cs_encrypt_cbc(out, tmpk, enclen);
    /* 帧尾附 TempKey[2..3] 8 字节 (不参与第一层加密) */
    memcpy(out + enclen, &tmpk[2], 8);
    /* 第二层: FrameKey CBC 加密全帧 */
    cs_encrypt_cbc(out, cs_frame_key, total);

    return cs_ppp_pack_in_place(out, total);
}

/* ------------------------------------------------------------------ */
/* 接收解密: FrameKey 解密 → 尾部 8B 重建 TempKey → TempKey 解密       */
/* ------------------------------------------------------------------ */
int cs_decrypt_rx(uint8_t *buf, size_t len)
{
    uint32_t tmpk[4];
    if (len < 16 || (len % 8) != 0) {
        return -1;
    }
    /* 第一层: FrameKey 解密全帧 */
    cs_decrypt_cbc(buf, cs_frame_key, len);
    /* 重建 TempKey: [0..1] = FrameKey[0..1]; [2..3] = 尾部 8 字节 */
    tmpk[0] = cs_frame_key[0];
    tmpk[1] = cs_frame_key[1];
    tmpk[2] = rd_le32(buf + len - 8);
    tmpk[3] = rd_le32(buf + len - 4);
    /* 第二层: TempKey 解密全帧 */
    cs_decrypt_cbc(buf, tmpk, len);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 帧解析 (解密后): 校验 header/CmdLen/CRC                             */
/* ------------------------------------------------------------------ */
int cs_frame_parse(const uint8_t *buf, size_t len,
                   uint8_t *cmd, uint16_t *param1, uint16_t *param2,
                   const uint8_t **data)
{
    uint16_t cmdlen;
    uint32_t crc_calc, crc_got;
    if (len < CS_CMD_MIN_LEN + 4) {
        return -1;
    }
    if (buf[CS_OFF_HEADER] != CS_HEADER) {
        return -1;
    }
    cmdlen = (uint16_t)(buf[CS_OFF_CMDLEN] | (buf[CS_OFF_CMDLEN + 1] << 8));
    if (cmdlen < CS_CMD_MIN_LEN || (cmdlen & 3) != 0) {
        return -1;
    }
    if ((size_t)cmdlen + 4 > len) {
        return -1;
    }
    crc_calc = cs_crc32_words(buf, cmdlen >> 2);
    crc_got = rd_le32(buf + cmdlen);
    if (crc_calc != crc_got) {
        return -2; /* CRC 校验失败 */
    }
    if (cmd)      *cmd = buf[CS_OFF_CMD];
    if (param1)   *param1 = (uint16_t)(buf[CS_OFF_PARAM1] | (buf[CS_OFF_PARAM1 + 1] << 8));
    if (param2)   *param2 = (uint16_t)(buf[CS_OFF_PARAM2] | (buf[CS_OFF_PARAM2 + 1] << 8));
    if (data)     *data = buf + CS_OFF_DATA;
    return 0;
}
