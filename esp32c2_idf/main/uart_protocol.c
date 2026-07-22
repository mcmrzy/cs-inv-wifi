#include "uart_protocol.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* 设置为 1 启用 UART1 原始数据打印（调试用） */
#define UART_RAW_DUMP 1

static const char *TAG = "uart_proto";

static FrameCallback g_frame_callback = NULL;

static FrameRxState g_rx_state = RX_WAIT_HEADER;
static uint8_t  g_rx_cmd = 0;
static uint16_t g_rx_data_len = 0;
static uint16_t g_rx_data_idx = 0;
static uint8_t  g_rx_data_buf[FRAME_DATA_MAX];
static uint8_t  g_rx_xor = 0;
static bool     g_rx_esc_next = false;
static SemaphoreHandle_t g_tx_mutex;
static SemaphoreHandle_t g_rx_mutex;

void uart_protocol_init() {
    const uart_config_t uart_config = {
        .baud_rate = UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(UART_PORT_NUM, &uart_config);
    uart_set_pin(UART_PORT_NUM, PIN_UART_TX, PIN_UART_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(UART_PORT_NUM, 1024, 512, 0, NULL, 0);
    g_tx_mutex = xSemaphoreCreateMutex();
    g_rx_mutex = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "UART%d TX=GPIO%d, RX=GPIO%d, baud=%d",
             UART_PORT_NUM, PIN_UART_TX, PIN_UART_RX, UART_BAUDRATE);
}

void uart_protocol_set_callback(FrameCallback cb) {
    g_frame_callback = cb;
}

static uint8_t calc_xor(uint8_t cmd, const uint8_t* data, uint16_t len) {
    uint8_t x = FRAME_HEADER ^ cmd ^ (uint8_t)((len >> 8) & 0xFF) ^ (uint8_t)(len & 0xFF);
    for (uint16_t i = 0; i < len; i++) {
        x ^= data[i];
    }
    return x;
}

static void uart_write_escaped(uint8_t b) {
    if (b == FRAME_HEADER) {
        uint8_t esc[2] = {FRAME_ESCAPE, FRAME_ESC_HEADER};
        uart_write_bytes(UART_PORT_NUM, esc, 2);
    } else if (b == FRAME_ESCAPE) {
        uint8_t esc[2] = {FRAME_ESCAPE, FRAME_ESC_ESCAPE};
        uart_write_bytes(UART_PORT_NUM, esc, 2);
    } else {
        uart_write_bytes(UART_PORT_NUM, &b, 1);
    }
}

bool uart_send_frame(uint8_t cmd, const uint8_t* data, uint16_t len) {
    if (len > FRAME_DATA_MAX || (len > 0 && !data) || !g_tx_mutex) return false;
    if (xSemaphoreTake(g_tx_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    uint8_t xor_val = calc_xor(cmd, data, len);
    uint8_t len_h = (len >> 8) & 0xFF;
    uint8_t len_l = len & 0xFF;

    uint8_t hdr = FRAME_HEADER;
    uart_write_bytes(UART_PORT_NUM, &hdr, 1);

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

bool uart_send_ack() {
    return uart_send_frame(CMD_ACK, NULL, 0);
}

bool uart_send_nack() {
    return uart_send_frame(CMD_NACK, NULL, 0);
}

void uart_protocol_process() {
    if (!g_rx_mutex || xSemaphoreTake(g_rx_mutex, 0) != pdTRUE) return;
    size_t available = 0;
    uart_get_buffered_data_len(UART_PORT_NUM, &available);

#if UART_RAW_DUMP
    /* 打印原始 hex 数据（不解析，纯调试） */
    if (available > 0) {
        uint8_t raw[256];
        int to_read = available > sizeof(raw) ? sizeof(raw) : (int)available;
        int got = uart_read_bytes(UART_PORT_NUM, raw, to_read, 0);
        char hex[770]; /* 256*3+1 */
        int pos = 0;
        for (int i = 0; i < got && pos < (int)sizeof(hex) - 4; i++) {
            pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", raw[i]);
        }
        ESP_LOGI(TAG, "UART1 RX [%d]: %s", got, hex);
        available -= got;
    }
#endif

    while (available > 0) {
        uint8_t b;
        int n = uart_read_bytes(UART_PORT_NUM, &b, 1, 0);
        if (n <= 0) break;
        available--;

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
    xSemaphoreGive(g_rx_mutex);
}
