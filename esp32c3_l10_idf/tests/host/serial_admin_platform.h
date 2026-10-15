#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct { uint32_t tx_buffer_size, rx_buffer_size; } usb_serial_jtag_driver_config_t;
#define CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG 1
#define CONFIG_CS_INV_ADMIN_PASSWORD "test-password"
#define ESP_OK 0
bool usb_serial_jtag_is_driver_installed(void);
int usb_serial_jtag_driver_install(usb_serial_jtag_driver_config_t *config);
int usb_serial_jtag_read_bytes(void *buf, uint32_t size, unsigned timeout);
bool ota_service_begin_config_change(void);
void ota_service_end_config_change(void);
void test_serial_log(const char *tag, const char *format, ...);
#define ESP_LOGI test_serial_log
#define ESP_LOGW test_serial_log
#define ESP_LOGE test_serial_log
