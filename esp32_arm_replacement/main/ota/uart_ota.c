/*
 * uart_ota.c — ARM OTA 帧收发实现 (UART1)
 *
 * 从 esp32c3_idf/uart_protocol.c 精简而来:
 *  - TX: uart_ota_send_frame (转义 + XOR, 互斥锁)
 *  - RX: uart_ota_rx_byte 单字节状态机 (由 arm_poll RX 任务喂入)
 */
#include "uart_ota.h"
#include "config.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "uart_ota";

typedef enum {
    RX_WAIT_HEADER,
    RX_RECV_CMD,
    RX_RECV_LEN_H,
    RX_RECV_LEN_L,
    RX_RECV_DATA,
    RX_RECV_XOR,
} FrameRxState;

static UartOtaFrameCallback g_frame_callback = NULL;
static FrameRxState g_rx_state = RX_WAIT_HEADER;
static uint8_t g_rx_cmd = 0;
static uint16_t g_rx_data_len = 0;
static uint16_t g_rx_data_idx = 0;
static uint8_t g_rx_data_buf[FRAME_DATA_MAX];
static uint8_t g_rx_xor = 0;
static bool g_rx_esc_next = false;
static SemaphoreHandle_t g_tx_mutex;

static uint8_t calc_xor(uint8_t cmd, const uint8_t *data, uint16_t len)
{
    uint8_t x = FRAME_HEADER ^ cmd ^ (uint8_t)((len >> 8) & 0xFF) ^
                (uint8_t)(len & 0xFF);
    for (uint16_t i = 0; i < len; i++) {
        x ^= data[i];
    }
    return x;
}

/* 串口写 (极性反转已由 uart_config_t.tx_invert 硬件处理) */
static void uart_raw_write(const uint8_t *p, size_t n)
{
    uart_write_bytes(UART_PORT_NUM, p, n);
}

static void uart_write_escaped(uint8_t b)
{
    if (b == FRAME_HEADER) {
        uint8_t esc[2] = {FRAME_ESCAPE, FRAME_ESC_HEADER};
        uart_raw_write(esc, 2);
    } else if (b == FRAME_ESCAPE) {
        uint8_t esc[2] = {FRAME_ESCAPE, FRAME_ESC_ESCAPE};
        uart_raw_write(esc, 2);
    } else {
        uart_raw_write(&b, 1);
    }
}

bool uart_ota_send_frame(uint8_t cmd, const uint8_t *data, uint16_t len)
{
    if (len > FRAME_DATA_MAX || (len > 0 && !data)) return false;
    if (!g_tx_mutex) {
        g_tx_mutex = xSemaphoreCreateMutex();
        if (!g_tx_mutex) return false;
    }
    if (xSemaphoreTake(g_tx_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;

    uint8_t xor_val = calc_xor(cmd, data, len);
    uint8_t len_h = (len >> 8) & 0xFF;
    uint8_t len_l = len & 0xFF;

    uint8_t hdr = FRAME_HEADER;
    uart_raw_write(&hdr, 1);
    uart_write_escaped(cmd);
    uart_write_escaped(len_h);
    uart_write_escaped(len_l);
    for (uint16_t i = 0; i < len; i++) {
        uart_write_escaped(data[i]);
    }
    uart_write_escaped(xor_val);

    xSemaphoreGive(g_tx_mutex);
    return true;
}

void uart_ota_rx_byte(uint8_t b)
{
    if (g_rx_state != RX_WAIT_HEADER && !g_rx_esc_next && b == FRAME_ESCAPE) {
        g_rx_esc_next = true;
        return;
    }
    if (g_rx_esc_next) {
        g_rx_esc_next = false;
        if (b == FRAME_ESC_HEADER) {
            b = FRAME_HEADER;
        } else if (b == FRAME_ESC_ESCAPE) {
            b = FRAME_ESCAPE;
        }
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
            ESP_LOGW(TAG, "Frame too long: %d bytes, dropped", g_rx_data_len);
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
            ESP_LOGW(TAG, "XOR error: got 0x%02X, expected 0x%02X, cmd=0x%02X",
                     b, g_rx_xor, g_rx_cmd);
        }
        break;
    }
}

void uart_ota_rx_reset(void)
{
    g_rx_state = RX_WAIT_HEADER;
    g_rx_esc_next = false;
    g_rx_data_idx = 0;
    g_rx_data_len = 0;
}

void uart_ota_set_callback(UartOtaFrameCallback cb)
{
    g_frame_callback = cb;
}
