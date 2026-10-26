/*
 * uart_ota.h — ARM OTA 帧收发 (UART1, 与 ARM 端 IAP 协议兼容)
 *
 * 帧格式与 esp32c3_idf/uart_protocol.c 一致:
 *   [0xAA header] [cmd] [len_h] [len_l] [data...] [xor]
 *   0xAA/0x55 转义, xor = header^cmd^len_h^len_l^data...
 *
 * RX 由 arm_poll 的 UART RX 任务喂字节 (uart_ota_rx_byte),
 * 整帧完成后回调 arm_ota_handle_uart。
 */
#ifndef UART_OTA_H
#define UART_OTA_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 帧回调: cmd + payload (len 字节) */
typedef void (*UartOtaFrameCallback)(uint8_t cmd, const uint8_t *data, uint16_t len);

/* 发送一帧 (含转义 + XOR 校验) */
bool uart_ota_send_frame(uint8_t cmd, const uint8_t *data, uint16_t len);

/* 喂入一个 RX 字节 (UART RX 任务调用), 完成整帧后触发回调 */
void uart_ota_rx_byte(uint8_t b);

/* 复位接收状态机 (OTA 取消/结束时调用, 丢弃残帧) */
void uart_ota_rx_reset(void);

/* 设置帧回调 */
void uart_ota_set_callback(UartOtaFrameCallback cb);

#ifdef __cplusplus
}
#endif

#endif /* UART_OTA_H */
