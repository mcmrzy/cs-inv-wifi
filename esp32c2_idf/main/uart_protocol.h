#ifndef UART_PROTOCOL_H
#define UART_PROTOCOL_H

#include "config.h"

typedef enum {
  RX_WAIT_HEADER,
  RX_RECV_CMD,
  RX_RECV_LEN_H,
  RX_RECV_LEN_L,
  RX_RECV_DATA,
  RX_RECV_XOR
} FrameRxState;

typedef void (*FrameCallback)(uint8_t cmd, const uint8_t* data, uint16_t len);

void uart_protocol_init();
void uart_protocol_set_callback(FrameCallback cb);
void uart_protocol_process();

bool uart_send_frame(uint8_t cmd, const uint8_t* data, uint16_t len);
bool uart_send_ack();
bool uart_send_nack();

#endif
