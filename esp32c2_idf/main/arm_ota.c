/**
 * ================================================================
 *  ARM OTA 模块实现 (ESP-IDF)
 *
 *  通过 UART 自定义帧协议，ESP32 给 GD32F303 ARM 芯片做固件升级。
 *  使用状态机 + esp_timer 超时，不阻塞 main loop。
 * ================================================================
 */

#include "arm_ota.h"
#include "uart_protocol.h"
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "ARM-OTA";

#define ARM_OTA_STREAM_BUF_SIZE  4096

static ArmOtaState       g_ota_state     = ARM_OTA_IDLE;
static ArmOtaCallback    g_ota_callback  = NULL;
static const uint8_t*    g_firmware      = NULL;
static size_t            g_firmware_len  = 0;

static uint16_t          g_total_packets = 0;
static uint16_t          g_current_packet = 0;
static int               g_retry_count   = 0;
static int               g_progress      = 0;

static int64_t           g_last_send_time  = 0;
static int64_t           g_state_enter_time = 0;

static char              g_message[128] = {0};

static ArmFirmwareInfo   g_fw_info;

static bool       g_streaming_mode    = false;
static size_t     g_stream_total      = 0;
static size_t     g_stream_sent       = 0;
static size_t     g_stream_buf_len    = 0;
static uint8_t    g_stream_buf[ARM_OTA_STREAM_BUF_SIZE];

static int64_t get_time_ms(void) {
    return esp_timer_get_time() / 1000;
}

static void set_state(ArmOtaState new_state, const char* msg) {
    g_ota_state = new_state;
    g_state_enter_time = get_time_ms();
    if (msg) {
        strncpy(g_message, msg, sizeof(g_message) - 1);
        g_message[sizeof(g_message) - 1] = '\0';
    }
    ESP_LOGI(TAG, "State=%d, %s", new_state, g_message);
    if (g_ota_callback) {
        g_ota_callback(new_state, g_progress, g_message);
    }
}

static void update_progress(void) {
    if (g_total_packets == 0) {
        g_progress = 0;
    } else {
        g_progress = (int)((uint32_t)g_current_packet * 100 / g_total_packets);
        if (g_progress > 100) g_progress = 100;
    }
}

static void put_u32_le(uint8_t* buf, uint32_t val) {
    buf[0] = (uint8_t)(val & 0xFF);
    buf[1] = (uint8_t)((val >> 8) & 0xFF);
    buf[2] = (uint8_t)((val >> 16) & 0xFF);
    buf[3] = (uint8_t)((val >> 24) & 0xFF);
}

static void put_u16_le(uint8_t* buf, uint16_t val) {
    buf[0] = (uint8_t)(val & 0xFF);
    buf[1] = (uint8_t)((val >> 8) & 0xFF);
}

static uint16_t get_u16_le(const uint8_t* buf) {
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

static uint32_t get_u32_le(const uint8_t* buf) {
    return (uint32_t)buf[0]
         | ((uint32_t)buf[1] << 8)
         | ((uint32_t)buf[2] << 16)
         | ((uint32_t)buf[3] << 24);
}

static bool send_ota_start(void) {
    uint8_t buf[4 + ARM_OTA_MD5_SIZE + 1 + 32];
    int pos = 0;

    put_u32_le(buf + pos, (uint32_t)g_firmware_len);
    pos += 4;

    memset(buf + pos, 0, ARM_OTA_MD5_SIZE);
    pos += ARM_OTA_MD5_SIZE;

    buf[pos++] = 0;

    bool ok = uart_send_frame(CMD_ARM_OTA_START, buf, pos);
    if (ok) {
        ESP_LOGI(TAG, "START sent: size=%u, packets=%u",
                 (unsigned)g_firmware_len, g_total_packets);
    }
    return ok;
}

static bool send_ota_data(uint16_t packet_idx) {
    static uint8_t buf[2 + ARM_OTA_PACKET_SIZE];
    int pos = 0;

    put_u16_le(buf + pos, packet_idx);
    pos += 2;

    size_t offset = (size_t)packet_idx * ARM_OTA_PACKET_SIZE;
    size_t remaining;
    size_t chunk;

    if (g_streaming_mode) {
        remaining = g_stream_total - offset;
        chunk = (remaining > ARM_OTA_PACKET_SIZE) ? ARM_OTA_PACKET_SIZE : remaining;
        chunk = (chunk > (size_t)g_stream_buf_len) ? (size_t)g_stream_buf_len : chunk;

        memcpy(buf + pos, g_stream_buf, chunk);
        pos += (int)chunk;

        g_stream_buf_len -= (int)chunk;
        if (g_stream_buf_len > 0) {
            memmove(g_stream_buf, g_stream_buf + chunk, g_stream_buf_len);
        }
    } else {
        remaining = g_firmware_len - offset;
        chunk = (remaining > ARM_OTA_PACKET_SIZE) ? ARM_OTA_PACKET_SIZE : remaining;
        memcpy(buf + pos, g_firmware + offset, chunk);
        pos += (int)chunk;
    }

    bool ok = uart_send_frame(CMD_ARM_OTA_DATA, buf, pos);
    if (ok) {
        ESP_LOGI(TAG, "DATA[%u/%u] sent, %u bytes",
                 packet_idx, g_total_packets, (unsigned)chunk);
    }
    return ok;
}

static bool send_ota_end(void) {
    uint8_t buf[2 + 4 + ARM_OTA_MD5_SIZE];
    int pos = 0;

    put_u16_le(buf + pos, g_total_packets);
    pos += 2;

    put_u32_le(buf + pos, (uint32_t)g_firmware_len);
    pos += 4;

    memset(buf + pos, 0, ARM_OTA_MD5_SIZE);
    pos += ARM_OTA_MD5_SIZE;

    bool ok = uart_send_frame(CMD_ARM_OTA_END, buf, pos);
    if (ok) {
        ESP_LOGI(TAG, "END sent: packets=%u, size=%u",
                 g_total_packets, (unsigned)g_firmware_len);
    }
    return ok;
}

static bool send_ota_info_request(void) {
    return uart_send_frame(CMD_ARM_OTA_INFO, NULL, 0);
}

static void process_idle(void) {
}

static void process_wait_ready(void) {
    int64_t now = get_time_ms();
    if (now - g_state_enter_time >= ARM_OTA_START_TIMEOUT) {
        set_state(ARM_OTA_TIMEOUT, "ARM IAP mode timeout");
    }
}

static void process_sending(void) {
    int64_t now = get_time_ms();
    if (now - g_last_send_time < ARM_OTA_SEND_INTERVAL) {
        return;
    }

    if (send_ota_data(g_current_packet)) {
        g_last_send_time = now;
        update_progress();
        set_state(ARM_OTA_WAIT_ACK, "Waiting for ACK");
    } else {
        set_state(ARM_OTA_ERROR, "UART send failed");
    }
}

static void process_wait_ack(void) {
    int64_t now = get_time_ms();
    if (now - g_state_enter_time >= ARM_OTA_ACK_TIMEOUT) {
        if (g_retry_count >= ARM_OTA_MAX_RETRIES) {
            char msg[64];
            snprintf(msg, sizeof(msg), "ACK timeout, packet %u, retries exhausted",
                     g_current_packet);
            set_state(ARM_OTA_ERROR, msg);
            return;
        }

        g_retry_count++;
        ESP_LOGW(TAG, "ACK timeout, retry %d/%d for packet %u",
                 g_retry_count, ARM_OTA_MAX_RETRIES, g_current_packet);
        set_state(ARM_OTA_SENDING, "Retrying packet");
    }
}

static void process_verifying(void) {
    int64_t now = get_time_ms();
    if (now - g_last_send_time < ARM_OTA_SEND_INTERVAL) {
        return;
    }

    if (send_ota_end()) {
        g_last_send_time = now;
        set_state(ARM_OTA_WAIT_READY, "Waiting for verify result");
    } else {
        set_state(ARM_OTA_ERROR, "Failed to send END command");
    }
}

static void stream_process(void) {
    if (g_ota_state != ARM_OTA_SENDING && g_ota_state != ARM_OTA_WAIT_ACK) {
        return;
    }

    int64_t now = get_time_ms();

    if (g_ota_state == ARM_OTA_WAIT_ACK) {
        if (now - g_state_enter_time >= ARM_OTA_ACK_TIMEOUT) {
            if (g_retry_count >= ARM_OTA_MAX_RETRIES) {
                set_state(ARM_OTA_TIMEOUT, "Stream ACK timeout");
                return;
            }
            g_retry_count++;
            ESP_LOGW(TAG, "Stream ACK timeout, retry %d/%d",
                     g_retry_count, ARM_OTA_MAX_RETRIES);
            set_state(ARM_OTA_SENDING, "Retrying packet");
        }
        return;
    }

    if (now - g_last_send_time < ARM_OTA_SEND_INTERVAL) {
        return;
    }

    size_t packet_offset = (size_t)g_current_packet * ARM_OTA_PACKET_SIZE;
    size_t remaining = g_stream_total - packet_offset;
    if (remaining == 0) {
        return;
    }

    size_t packet_len = (remaining > ARM_OTA_PACKET_SIZE) ? ARM_OTA_PACKET_SIZE : remaining;

    if (send_ota_data(g_current_packet)) {
        g_last_send_time = now;
        g_stream_sent += packet_len;
        g_progress = (int)((uint32_t)g_stream_sent * 100 / g_stream_total);
        update_progress();
        set_state(ARM_OTA_WAIT_ACK, "Waiting for ACK");
    }
}

bool arm_ota_start_streaming(size_t total_size) {
    if (total_size == 0) {
        ESP_LOGE(TAG, "Error: invalid total size");
        return false;
    }

    if (g_ota_state != ARM_OTA_IDLE && g_ota_state != ARM_OTA_DONE
        && g_ota_state != ARM_OTA_ERROR && g_ota_state != ARM_OTA_TIMEOUT) {
        ESP_LOGW(TAG, "Cannot start streaming in state %d", g_ota_state);
        return false;
    }

    g_streaming_mode = true;
    g_stream_total = total_size;
    g_stream_sent = 0;
    g_stream_buf_len = 0;
    g_current_packet = 0;
    g_retry_count = 0;
    g_progress = 0;
    g_total_packets = (uint16_t)((total_size + ARM_OTA_PACKET_SIZE - 1) / ARM_OTA_PACKET_SIZE);

    ESP_LOGI(TAG, "Streaming mode: size=%u, packets=%u",
             (unsigned)total_size, g_total_packets);

    g_firmware_len = total_size;
    if (send_ota_start()) {
        g_last_send_time = get_time_ms();
        set_state(ARM_OTA_WAIT_READY, "Waiting for ARM IAP ready");
        return true;
    } else {
        g_streaming_mode = false;
        set_state(ARM_OTA_ERROR, "Failed to send START");
        return false;
    }
}

bool arm_ota_feed(const uint8_t* data, size_t len) {
    if (!g_streaming_mode || len == 0) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        if (g_stream_buf_len < ARM_OTA_STREAM_BUF_SIZE) {
            g_stream_buf[g_stream_buf_len++] = data[i];
        }
    }

    return true;
}

bool arm_ota_end_streaming(void) {
    if (!g_streaming_mode) {
        return false;
    }

    if (g_stream_buf_len > 0 && g_current_packet < g_total_packets) {
        int64_t now = get_time_ms();
        while (g_current_packet < g_total_packets) {
            size_t packet_offset = (size_t)g_current_packet * ARM_OTA_PACKET_SIZE;
            size_t remaining = g_stream_total - packet_offset;
            if (remaining == 0) break;

            if (now - g_last_send_time < ARM_OTA_SEND_INTERVAL) {
                int64_t delay_ms = ARM_OTA_SEND_INTERVAL - (now - g_last_send_time);
                vTaskDelay(pdMS_TO_TICKS(delay_ms));
            }

            if (!send_ota_data(g_current_packet)) {
                ESP_LOGE(TAG, "Failed to send last packet");
                return false;
            }
            g_last_send_time = get_time_ms();
            now = g_last_send_time;
        }
    }

    if (send_ota_end()) {
        g_last_send_time = get_time_ms();
        set_state(ARM_OTA_WAIT_READY, "Waiting for verify result");
        return true;
    }
    return false;
}

bool arm_ota_is_streaming_mode(void) {
    return g_streaming_mode;
}

void arm_ota_init(void) {
    g_ota_state = ARM_OTA_IDLE;
    g_ota_callback = NULL;
    g_firmware = NULL;
    g_firmware_len = 0;
    g_total_packets = 0;
    g_current_packet = 0;
    g_retry_count = 0;
    g_progress = 0;
    g_last_send_time = 0;
    g_state_enter_time = 0;
    g_message[0] = '\0';
    memset(&g_fw_info, 0, sizeof(g_fw_info));
    g_fw_info.valid = false;
    g_streaming_mode = false;
    g_stream_total = 0;
    g_stream_sent = 0;
    g_stream_buf_len = 0;
    memset(g_stream_buf, 0, sizeof(g_stream_buf));

    ESP_LOGI(TAG, "Module initialized");
}

void arm_ota_set_callback(ArmOtaCallback cb) {
    g_ota_callback = cb;
}

bool arm_ota_start(const uint8_t* firmware, size_t length) {
    if (!firmware || length == 0) {
        ESP_LOGE(TAG, "Error: invalid firmware data");
        return false;
    }

    if (g_ota_state != ARM_OTA_IDLE && g_ota_state != ARM_OTA_DONE
        && g_ota_state != ARM_OTA_ERROR && g_ota_state != ARM_OTA_TIMEOUT) {
        ESP_LOGE(TAG, "Error: cannot start in state %d", g_ota_state);
        return false;
    }

    g_firmware = firmware;
    g_firmware_len = length;
    g_current_packet = 0;
    g_retry_count = 0;
    g_progress = 0;

    g_total_packets = (uint16_t)((length + ARM_OTA_PACKET_SIZE - 1) / ARM_OTA_PACKET_SIZE);

    ESP_LOGI(TAG, "Starting OTA: size=%u, packets=%u",
             (unsigned)length, g_total_packets);

    if (send_ota_start()) {
        g_last_send_time = get_time_ms();
        set_state(ARM_OTA_WAIT_READY, "Waiting for ARM IAP ready");
        return true;
    } else {
        set_state(ARM_OTA_ERROR, "Failed to send START command");
        return false;
    }
}

void arm_ota_process(void) {
    switch (g_ota_state) {
        case ARM_OTA_IDLE:
            process_idle();
            break;

        case ARM_OTA_WAIT_READY:
            process_wait_ready();
            break;

        case ARM_OTA_SENDING:
            if (g_streaming_mode) {
                stream_process();
            } else {
                process_sending();
            }
            break;

        case ARM_OTA_WAIT_ACK:
            if (g_streaming_mode) {
                stream_process();
            } else {
                process_wait_ack();
            }
            break;

        case ARM_OTA_VERIFYING:
            process_verifying();
            break;

        case ARM_OTA_DONE:
        case ARM_OTA_ERROR:
        case ARM_OTA_TIMEOUT:
            break;

        default:
            break;
    }
}

ArmOtaState arm_ota_get_state(void) {
    return g_ota_state;
}

int arm_ota_get_progress(void) {
    return g_progress;
}

const char* arm_ota_get_message(void) {
    return g_message;
}

bool arm_ota_is_ready(void) {
    return (g_ota_state == ARM_OTA_IDLE);
}

bool arm_ota_request_info(void) {
    if (g_ota_state != ARM_OTA_IDLE) {
        ESP_LOGW(TAG, "Cannot request info in state %d", g_ota_state);
        return false;
    }

    g_fw_info.valid = false;
    bool ok = send_ota_info_request();
    if (ok) {
        ESP_LOGI(TAG, "INFO request sent");
    }
    return ok;
}

const ArmFirmwareInfo* arm_ota_get_firmware_info(void) {
    return &g_fw_info;
}

void arm_ota_cancel(void) {
    if (g_ota_state == ARM_OTA_IDLE) {
        return;
    }
    ESP_LOGW(TAG, "Cancelled from state %d", g_ota_state);
    g_firmware = NULL;
    g_firmware_len = 0;
    g_progress = 0;
    set_state(ARM_OTA_IDLE, "OTA cancelled");
}

void arm_ota_handle_uart(uint8_t cmd, const uint8_t* data, uint16_t len) {
    switch (cmd) {
        case CMD_ARM_OTA_ACK: {
            ESP_LOGI(TAG, "ACK received, len=%u", len);

            if (g_ota_state == ARM_OTA_WAIT_READY) {
                g_current_packet = 0;
                g_retry_count = 0;
                set_state(ARM_OTA_SENDING, "Sending firmware data");
            }
            else if (g_ota_state == ARM_OTA_WAIT_ACK) {
                g_retry_count = 0;
                g_current_packet++;

                if (g_current_packet >= g_total_packets) {
                    g_progress = 100;
                    set_state(ARM_OTA_VERIFYING, "All packets sent, verifying");
                } else {
                    update_progress();
                    set_state(ARM_OTA_SENDING, "Sending next packet");
                }
            }
            else if (g_ota_state == ARM_OTA_VERIFYING) {
            }
            else if (g_ota_state == ARM_OTA_WAIT_READY && g_progress == 100) {
                set_state(ARM_OTA_DONE, "OTA completed successfully");
                g_firmware = NULL;
                g_firmware_len = 0;
            }
            break;
        }

        case CMD_ARM_OTA_NACK: {
            uint16_t nack_packet = 0;
            if (len >= 2) {
                nack_packet = get_u16_le(data);
            }

            ESP_LOGW(TAG, "NACK received, packet=%u", nack_packet);

            if (g_ota_state == ARM_OTA_WAIT_ACK) {
                if (g_retry_count >= ARM_OTA_MAX_RETRIES) {
                    char msg[64];
                    snprintf(msg, sizeof(msg), "NACK max retries for packet %u", nack_packet);
                    set_state(ARM_OTA_ERROR, msg);
                } else {
                    g_retry_count++;
                    if (nack_packet < g_total_packets) {
                        g_current_packet = nack_packet;
                    }
                    char msg[64];
                    snprintf(msg, sizeof(msg), "NACK retry %d/%d for packet %u",
                             g_retry_count, ARM_OTA_MAX_RETRIES, g_current_packet);
                    set_state(ARM_OTA_SENDING, msg);
                }
            }
            break;
        }

        case CMD_ARM_OTA_INFO: {
            ESP_LOGI(TAG, "INFO received, len=%u", len);

            memset(&g_fw_info, 0, sizeof(g_fw_info));
            g_fw_info.valid = false;

            if (len < 1 + 4 + ARM_OTA_MD5_SIZE) {
                ESP_LOGW(TAG, "INFO data too short");
                break;
            }

            int pos = 0;

            uint8_t ver_len = data[pos++];
            if (ver_len > sizeof(g_fw_info.version) - 1) {
                ver_len = sizeof(g_fw_info.version) - 1;
            }

            if (pos + ver_len + 4 + ARM_OTA_MD5_SIZE > len) {
                ESP_LOGW(TAG, "INFO data length mismatch");
                break;
            }
            memcpy(g_fw_info.version, data + pos, ver_len);
            g_fw_info.version[ver_len] = '\0';
            pos += ver_len;

            g_fw_info.fw_size = get_u32_le(data + pos);
            pos += 4;

            memcpy(g_fw_info.md5, data + pos, ARM_OTA_MD5_SIZE);
            pos += ARM_OTA_MD5_SIZE;

            g_fw_info.valid = true;

            ESP_LOGI(TAG, "ARM firmware: version=\"%s\", size=%u",
                     g_fw_info.version, (unsigned)g_fw_info.fw_size);
            break;
        }

        default:
            break;
    }
}
