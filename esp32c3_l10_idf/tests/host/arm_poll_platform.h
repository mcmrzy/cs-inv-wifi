#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void *SemaphoreHandle_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef void (*TaskFunction_t)(void *);
typedef struct {
    int baud_rate, data_bits, parity, stop_bits, flow_ctrl, source_clk;
} uart_config_t;

#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define UART_DATA_8_BITS 8
#define UART_PARITY_DISABLE 0
#define UART_STOP_BITS_1 1
#define UART_HW_FLOWCTRL_DISABLE 0
#define UART_SCLK_DEFAULT 0
#define UART_PORT_NUM 0
#define UART_BAUDRATE 115200
#define PIN_UART_TX 6
#define PIN_UART_RX 7
#define UART_INVERT_TX 0
#define UART_INVERT_RX 0
#define UART_SIGNAL_TXD_INV 1
#define UART_SIGNAL_RXD_INV 2
#define ARM_OTA_IDLE 0
#define SN_MAX_LEN 32
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOG_BUFFER_HEX(...) ((void)0)
#define ESP_LOG_BUFFER_HEX_LEVEL(...) ((void)0)
#define ESP_LOG_DEBUG 0
#define ESP_ERROR_CHECK(expr) ((void)(expr))

extern char g_device_sn[SN_MAX_LEN];
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, uint32_t timeout);
BaseType_t xSemaphoreGive(SemaphoreHandle_t handle);
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack,
                       void *arg, UBaseType_t priority, void *handle);
int uart_driver_install(int port, int rx, int tx, int count, void *queue, int flags);
int uart_param_config(int port, const uart_config_t *cfg);
int uart_set_pin(int port, int tx, int rx, int rts, int cts);
int uart_set_line_inverse(int port, uint32_t mask);
int uart_write_bytes(int port, const void *data, size_t len);
int uart_read_bytes(int port, void *data, size_t len, uint32_t timeout);
int64_t esp_timer_get_time(void);
uint32_t esp_random(void);
int arm_ota_get_state(void);
bool arm_ota_is_streaming_mode(void);
void arm_ota_rx_reset(void);
void arm_ota_rx_byte(uint8_t byte);
void mqtt_clear_retained_status(const char *sn);
bool storage_set_sn(const char *sn);
size_t strlcpy(char *dst, const char *src, size_t capacity);
