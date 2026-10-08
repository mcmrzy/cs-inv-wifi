/*
 * cs_protocol.h — CS-L10-6K2 采集器串口协议 (ESP32 ↔ ARM)
 *
 * 与 ARM 端源码 (Collector.c / ComProtocol.c / TEA.c / CRC32.c) 逐字节对齐：
 *  - 帧结构: 0xAA + CmdLen(u16,4的倍数) + CMD(u8) + Param1(u16) + Param2(u16) + Data[]
 *  - CRC32 存储在帧偏移 CmdLen 处(帧外 4 字节, 小端)
 *  - 加密: TEA 标准 32 轮 delta=0x9e3779b9, CBC 链模式(InitV=0)
 *  - 发送加密: TempKey(FrameKey[0..1] + 随机 8B) 加密 EncryptLen 字节 →
 *             帧尾附 TempKey[2..3] 8B → FrameKey 加密 (EncryptLen+8) 字节
 *  - 接收解密: FrameKey 解密全帧 → 尾部 8B 重建 TempKey → TempKey 解密全帧
 *  - PPP: 0x7E 定界, 0x7E->0x7D 0x5E, 0x7D->0x7D 0x5D
 */
#ifndef CS_PROTOCOL_H
#define CS_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 命令码 (Collector.h) ---- */
#define CS_CMD_READ_CTRLPARAM   0x01
#define CS_CMD_WRITE_CTRLPARAM  0x02
#define CS_CMD_READ_RUNPARAM    0x03
#define CS_CMD_READ_R_W_DATA    0x04
#define CS_CMD_WRITE_R_W_DATA   0x05
#define CS_CMD_READ_R_DATA      0x06
#define CS_CMD_CTRLCMD          0x07
#define CS_CMD_READ_BATTSUM     0x08
#define CS_CMD_READ_TEST_RUNPARAM 0x14

#define CS_BATTSUM_LAYOUT       0
#define CS_BATTSUM_DATA_SIZE    100

/* ---- 帧常量 (ComProtocol.h) ---- */
#define CS_HEADER           0xAA
#define CS_FRAME_MAX        1088    /* FRAMLEN: CS 轮询/遥测/接收帧上限 */
/* 发送帧构建上限: IAP 4KB 包(param2=4114)的 total≈4136B。发送侧直接在调用方
 * 输出缓冲内构建并倒序 PPP 转义；接收侧 boot 应答仍受 CS_FRAME_MAX 约束。 */
#define CS_FRAME_TX_MAX     4352
#define CS_CMD_MIN_LEN      8
#define CS_PPP_ESC_7E       0x7E
#define CS_PPP_ESC_7D       0x7D
#define CS_PPP_ESC_5E       0x5E
#define CS_PPP_ESC_5D       0x5D

/* 帧内偏移 (CollectorCmdDef, pack(1)) */
#define CS_OFF_HEADER   0
#define CS_OFF_CMDLEN   1
#define CS_OFF_CMD      3
#define CS_OFF_PARAM1   4
#define CS_OFF_PARAM2   6
#define CS_OFF_DATA     8

/* FrameKey (TEA.c 源码公开) */
extern const uint32_t cs_frame_key[4];

/* ---- CRC32 (CRC32.c, 逐 32 位字, MSB-first, poly 0x4C11DB7, init 0xFFFFFFFF, 无最终异或) ---- */
uint32_t cs_crc32_words(const uint8_t *buf, size_t word_count);

/* ---- TEA CBC (TEA.c) ---- */
void cs_tea_encrypt_block(uint32_t v[2], const uint32_t k[4]);
void cs_tea_decrypt_block(uint32_t v[2], const uint32_t k[4]);
/* len 必须为 8 的整数倍 */
void cs_encrypt_cbc(uint8_t *buf, const uint32_t *key, size_t len);
void cs_decrypt_cbc(uint8_t *buf, const uint32_t *key, size_t len);

/* ---- PPP (ComProtocol.c) ---- */
/* 封装: dst 容量需 >= len*2+2, 返回封装后长度 */
size_t cs_ppp_pack(uint8_t *dst, const uint8_t *src, size_t len);
/* 计算封装后长度 */
size_t cs_ppp_pack_len(size_t len);

/* PPP 接收状态机 (PPPFrameCmdReceive) */
typedef struct {
    uint8_t  status;    /* 0=WAITRECEVIE 1=ISRCVING 2=RCVFINISH */
    uint8_t  flg7e;
    uint16_t length;
    uint8_t  buf[CS_FRAME_MAX];
} cs_ppp_rx_t;

#define CS_PPP_RX_WAIT      0
#define CS_PPP_RX_RCVING    1
#define CS_PPP_RX_FINISH    2

void cs_ppp_rx_init(cs_ppp_rx_t *rx);
/* 喂一个字节; 返回 1 表示一帧接收完成(数据在 rx->buf, 长度 rx->length), 否则 0 */
int cs_ppp_rx_byte(cs_ppp_rx_t *rx, uint8_t b);

/* ---- 帧收发 (Collector.c 发送/接收逻辑) ---- */
/*
 * 构建并加密发送帧:
 *  out      输出缓冲区(PPP 封装后), 容量需 >= cs_frame_send_len()
 *  cmd      CS_CMD_*
 *  param1   起始地址
 *  param2   数据长度(字节)
 *  data     param2 字节数据, 可为 NULL(读命令)
 *  rand     TempKey 随机数 (ARM 端用 SysTick->VAL; ESP32 用 esp_random())
 * 返回: 封装后总长度
 */
size_t cs_frame_send_len(uint16_t param2);
size_t cs_frame_send(uint8_t *out, uint8_t cmd, uint16_t param1,
                     uint16_t param2, const uint8_t *data, uint32_t rand);

/*
 * 解析接收帧 (已 PPP 解封装、未解密):
 *  调用方需先 cs_decrypt_rx() 完成两次解密
 */
int cs_frame_parse(const uint8_t *buf, size_t len,
                   uint8_t *cmd, uint16_t *param1, uint16_t *param2,
                   const uint8_t **data);

/* Validate a decrypted layout-0 response, including its exact payload size. */
int cs_frame_parse_bms_summary(const uint8_t *buf, size_t len,
                              const uint8_t **data);

/* 接收解密: 两次 CBC 解密 (FrameKey → 尾部 8B 重建 TempKey → TempKey), 返回 0=成功 */
int cs_decrypt_rx(uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* CS_PROTOCOL_H */
