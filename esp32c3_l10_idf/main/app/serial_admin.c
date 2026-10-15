#include "serial_admin.h"
#include "admin_sn_command.h"
#include "arm_poll.h"
#ifdef HOST_TEST
#include "serial_admin_platform.h"
#else
#include "sdkconfig.h"
#include "esp_log.h"
#include "driver/usb_serial_jtag.h"
#include "ota_service.h"
#endif

static const char *TAG = "SERIAL-ADMIN";
static admin_sn_input_t s_input;
static bool s_enabled, s_pending, s_reboot_pending;

void serial_admin_init(void)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    if (!CONFIG_CS_INV_ADMIN_PASSWORD[0]) {
        ESP_LOGW(TAG, "SN command disabled: administrator password is empty");
        return;
    }
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t cfg = { .tx_buffer_size = 256, .rx_buffer_size = 512 };
        if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
            ESP_LOGE(TAG, "USB console input unavailable");
            return;
        }
    }
    s_enabled = true;
    ESP_LOGI(TAG, "USB command: set_sn <admin_password> <SN:1..16 alphanumeric>");
#else
    ESP_LOGW(TAG, "SN command requires USB Serial/JTAG console");
#endif
}

static const char *result_name(arm_sn_result_t result)
{
    switch (result) {
    case ARM_SN_OK: return "OK";
    case ARM_SN_REJECTED: return "ARM_REJECTED";
    case ARM_SN_TIMEOUT: return "ARM_TIMEOUT";
    case ARM_SN_MISMATCH: return "READBACK_MISMATCH";
    case ARM_SN_NVS_FAILED: return "NVS_FAILED: ARM updated, retry to save ESP SN";
    case ARM_SN_TX_FAILED: return "UART_TX_FAILED";
    case ARM_SN_INTERRUPTED: return "INTERRUPTED";
    default: return "INTERNAL_ERROR";
    }
}

void serial_admin_tick(void)
{
    if (!s_enabled) return;
    if (s_pending) {
        arm_sn_result_t result = arm_poll_sn_result();
        if (result != ARM_SN_PENDING) {
            s_pending = false;
            if (result == ARM_SN_OK) {
                s_reboot_pending = true;
                /* Retain reservation: MQTT/BLE identity will change on restart. */
                ESP_LOGI(TAG, "set_sn OK: ARM readback and ESP NVS confirmed; restarting");
            } else {
                ota_service_end_config_change();
                ESP_LOGW(TAG, "set_sn %s; ESP SN unchanged; retry command", result_name(result));
            }
        }
    }
    uint8_t bytes[64];
    /* At most 256 input bytes per call. No stdio readline/echo. */
    for (unsigned chunk = 0; chunk < 4; chunk++) {
        int count = usb_serial_jtag_read_bytes(bytes, sizeof(bytes), 0);
        if (count <= 0) break;
        for (int i = 0; i < count; i++) {
            char sn[17] = {0};
            admin_sn_result_t r = admin_sn_input_byte(&s_input, bytes[i],
                                                      CONFIG_CS_INV_ADMIN_PASSWORD, sn);
            if (r == ADMIN_SN_VALID) {
                if (s_pending || s_reboot_pending || !ota_service_begin_config_change()) {
                    ESP_LOGW(TAG, "set_sn BUSY");
                } else if (arm_poll_request_sn(sn) != 0) {
                    ota_service_end_config_change();
                    ESP_LOGW(TAG, "set_sn BUSY: ARM transaction in progress");
                } else {
                    s_pending = true;
                    ESP_LOGI(TAG, "set_sn ACCEPTED: waiting for ARM write and readback");
                }
            } else if (r == ADMIN_SN_AUTH_FAILED) {
                ESP_LOGW(TAG, "set_sn AUTH_FAILED");
            } else if (r == ADMIN_SN_INVALID_ARGS || r == ADMIN_SN_TOO_LONG) {
                ESP_LOGW(TAG, "set_sn %s", r == ADMIN_SN_TOO_LONG ? "LINE_TOO_LONG" : "INVALID_ARGS");
            }
            bytes[i] = 0;
        }
    }
}
