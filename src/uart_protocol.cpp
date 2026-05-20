#include "uart_protocol.h"

// ==================== 全局状态 ====================
static FrameCallback g_frame_callback = nullptr;

static FrameRxState g_rx_state = RX_WAIT_HEADER;
static uint8_t  g_rx_cmd = 0;
static uint16_t g_rx_data_len = 0;
static uint16_t g_rx_data_idx = 0;
static uint8_t  g_rx_data_buf[FRAME_DATA_MAX];
static uint8_t  g_rx_xor = 0;
static bool     g_rx_esc_next = false;  // 下一个字节需要转义解码

// ESP32-C3 (ARDUINO_USB_CDC_ON_BOOT=1):
//   Serial  = USB CDC 虚拟串口（调试打印，连电脑）
//   Serial1 = Hardware UART1（可自定义 GPIO，用于 ARM 通信）
// ESP8266:
//   Serial = Hardware UART（调试+ARM通信共用）

#ifdef CHIP_ESP32C3
  #define UART_SERIAL Serial1
#else
  #define UART_SERIAL Serial
#endif

// ==================== 初始化 ====================
void uart_protocol_init() {
#ifdef CHIP_ESP32C3
  Serial1.begin(UART_BAUDRATE, SERIAL_8N1, PIN_UART_RX, PIN_UART_TX);
  Serial.printf("[UART] ESP32-C3: Serial1 TX=GPIO%d, RX=GPIO%d, baud=%d\n", 
                PIN_UART_TX, PIN_UART_RX, UART_BAUDRATE);
#else
  Serial.begin(UART_BAUDRATE, SERIAL_8N1, SERIAL_FULL, PIN_UART_TX);
  Serial.printf("[UART] ESP8266: TX=GPIO%d, RX=GPIO%d, baud=%d\n", PIN_UART_TX, PIN_UART_RX, UART_BAUDRATE);
#endif
}

void uart_protocol_set_callback(FrameCallback cb) {
  g_frame_callback = cb;
}

// ==================== 发送 ====================
static uint8_t calc_xor(uint8_t cmd, const uint8_t* data, uint16_t len) {
  uint8_t x = FRAME_HEADER ^ cmd ^ (uint8_t)((len >> 8) & 0xFF) ^ (uint8_t)(len & 0xFF);
  for (uint16_t i = 0; i < len; i++) {
    x ^= data[i];
  }
  return x;
}

static void uart_write_escaped(uint8_t b) {
  if (b == FRAME_HEADER)      { UART_SERIAL.write(FRAME_ESCAPE); UART_SERIAL.write(FRAME_ESC_HEADER); }
  else if (b == FRAME_ESCAPE) { UART_SERIAL.write(FRAME_ESCAPE); UART_SERIAL.write(FRAME_ESC_ESCAPE); }
  else                         { UART_SERIAL.write(b); }
}

bool uart_send_frame(uint8_t cmd, const uint8_t* data, uint16_t len) {
  uint8_t xor_val = calc_xor(cmd, data, len);
  uint8_t len_h = (len >> 8) & 0xFF;
  uint8_t len_l = len & 0xFF;

  UART_SERIAL.write(FRAME_HEADER);

  uart_write_escaped(cmd);
  uart_write_escaped(len_h);
  uart_write_escaped(len_l);

  for (uint16_t i = 0; i < len; i++) {
    uart_write_escaped(data[i]);
  }
  uart_write_escaped(xor_val);

  return true;
}

bool uart_send_ack() {
  uint8_t dummy = 0;
  return uart_send_frame(CMD_ACK, &dummy, 1);
}

bool uart_send_nack() {
  uint8_t dummy = 0;
  return uart_send_frame(CMD_NACK, &dummy, 1);
}

// ==================== 接收处理 ====================
void uart_protocol_process() {
  while (UART_SERIAL.available() > 0) {
    uint8_t b = UART_SERIAL.read();

    // 转义解码（HEADER 之后的所有字节都需要检查转义）
    if (g_rx_state != RX_WAIT_HEADER && !g_rx_esc_next && b == FRAME_ESCAPE) {
      g_rx_esc_next = true;
      continue;
    }
    if (g_rx_esc_next) {
      g_rx_esc_next = false;
      if (b == FRAME_ESC_HEADER) {
        b = FRAME_HEADER;
      } else if (b == FRAME_ESC_ESCAPE) {
        b = FRAME_ESCAPE;
      }
      // 其他值保持原样
    }

    switch (g_rx_state) {
      case RX_WAIT_HEADER:
        if (b == FRAME_HEADER) {
          g_rx_state = RX_RECV_CMD;
          g_rx_xor = FRAME_HEADER;
          g_rx_esc_next = false;
        }
        break;

      case RX_RECV_CMD:
        g_rx_cmd = b;
        g_rx_xor ^= b;
        g_rx_state = RX_RECV_LEN_H;
        break;

      case RX_RECV_LEN_H:
        g_rx_data_len = ((uint16_t)b) << 8;
        g_rx_xor ^= b;
        g_rx_state = RX_RECV_LEN_L;
        break;

      case RX_RECV_LEN_L:
        g_rx_data_len |= b;
        g_rx_xor ^= b;
        g_rx_data_idx = 0;

        if (g_rx_data_len == 0) {
          g_rx_state = RX_RECV_XOR;
        } else if (g_rx_data_len > FRAME_DATA_MAX) {
          Serial.printf("[UART] Frame too long: %d bytes, dropped\n", g_rx_data_len);
          g_rx_state = RX_WAIT_HEADER;
        } else {
          g_rx_state = RX_RECV_DATA;
        }
        break;

      case RX_RECV_DATA:
        g_rx_data_buf[g_rx_data_idx++] = b;
        g_rx_xor ^= b;
        if (g_rx_data_idx >= g_rx_data_len) {
          g_rx_state = RX_RECV_XOR;
        }
        break;

      case RX_RECV_XOR:
        g_rx_state = RX_WAIT_HEADER;
        if (b == g_rx_xor) {
          if (g_frame_callback) {
            g_frame_callback(g_rx_cmd, g_rx_data_buf, g_rx_data_len);
          }
        } else {
          Serial.printf("[UART] XOR error: got 0x%02X, expected 0x%02X, cmd=0x%02X\n",
                        b, g_rx_xor, g_rx_cmd);
        }
        break;
    }
  }
}
