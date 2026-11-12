/* Minimal deterministic SDK boundary for the actual diagnostic command runner. */
#pragma once
#include <stdlib.h>
typedef int esp_err_t;
typedef int gpio_num_t;
typedef void *QueueHandle_t;
typedef unsigned TickType_t;
typedef int esp_ota_img_states_t;
typedef struct {int unused;} esp_partition_t;
typedef struct {const char *version;} esp_app_desc_t;
typedef struct {unsigned tx_buffer_size,rx_buffer_size;} usb_serial_jtag_driver_config_t;
typedef struct {int baud_rate,data_bits,parity,stop_bits,flow_ctrl,source_clk;} uart_config_t;
typedef struct {int type;size_t size;} uart_event_t;
enum {ESP_OK=0,ESP_ERR_TIMEOUT=1,ESP_LOG_NONE=0,ESP_LOG_WARN=2,ESP_OTA_IMG_PENDING_VERIFY=1,
      UART_NUM_0=0,GPIO_NUM_0=0,GPIO_NUM_1=1,UART_DATA_8_BITS=8,
      UART_PARITY_DISABLE=0,UART_PARITY_EVEN=1,UART_PARITY_ODD=2,
      UART_STOP_BITS_1=1,UART_STOP_BITS_2=2,UART_HW_FLOWCTRL_DISABLE=0,UART_SCLK_DEFAULT=0,
      UART_PIN_NO_CHANGE=-1,UART_SIGNAL_RXD_INV=4,UART_SIGNAL_TXD_INV=32,
      UART_PARITY_ERR=1,UART_FRAME_ERR=2,UART_FIFO_OVF=3,UART_BUFFER_FULL=4,UART_BREAK=5,
      pdTRUE=1,pdFALSE=0};
#define pdMS_TO_TICKS(n) (n)
#define ESP_ERROR_CHECK(e) do {if((e)!=ESP_OK)abort();} while(0)
extern uint32_t mock_ms;
int64_t esp_timer_get_time(void);
int usb_serial_jtag_write_bytes(const void *,size_t,unsigned);
int usb_serial_jtag_read_bytes(void *,uint32_t,unsigned);
esp_err_t usb_serial_jtag_driver_install(usb_serial_jtag_driver_config_t *);
esp_err_t usb_serial_jtag_wait_tx_done(unsigned);
int xQueueReceive(QueueHandle_t,void *,unsigned);
void vTaskDelay(unsigned);
int gpio_get_level(gpio_num_t);
esp_err_t gpio_reset_pin(gpio_num_t);
esp_err_t uart_wait_tx_done(int,unsigned);
esp_err_t uart_param_config(int,const uart_config_t *);
esp_err_t uart_set_pin(int,int,int,int,int);
esp_err_t uart_set_line_inverse(int,uint32_t);
esp_err_t uart_flush_input(int);
esp_err_t uart_driver_install(int,int,int,int,QueueHandle_t *,int);
int uart_write_bytes(int,const void *,size_t);
int uart_read_bytes(int,void *,uint32_t,unsigned);
esp_err_t uart_get_buffered_data_len(int,size_t *);
esp_err_t uart_get_baudrate(int,uint32_t *);
const char *esp_err_to_name(esp_err_t);
const esp_app_desc_t *esp_app_get_description(void);
uint32_t esp_get_free_heap_size(void);
unsigned uxTaskGetStackHighWaterMark(void *);
void esp_restart(void);
void esp_log_level_set(const char *,int);
const esp_partition_t *esp_ota_get_running_partition(void);
esp_err_t esp_ota_get_state_partition(const esp_partition_t *,esp_ota_img_states_t *);
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void);
