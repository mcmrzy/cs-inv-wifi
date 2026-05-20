#ifndef UART_PROTOCOL_H
#define UART_PROTOCOL_H

#include "config.h"

// ==================== 帧解析状态 ====================
enum FrameRxState {
  RX_WAIT_HEADER,
  RX_RECV_CMD,
  RX_RECV_LEN_H,
  RX_RECV_LEN_L,
  RX_RECV_DATA,
  RX_RECV_XOR
};

// ==================== 帧回调 ====================
// 当收到完整帧时调用：cmd=命令码, data=数据指针, len=数据长度
typedef void (*FrameCallback)(uint8_t cmd, const uint8_t* data, uint16_t len);

// ==================== API ====================
void uart_protocol_init();
void uart_protocol_set_callback(FrameCallback cb);
void uart_protocol_process();  // 在 loop() 中调用

// 发送帧到 ARM MCU
bool uart_send_frame(uint8_t cmd, const uint8_t* data, uint16_t len);
bool uart_send_ack();
bool uart_send_nack();

#endif
