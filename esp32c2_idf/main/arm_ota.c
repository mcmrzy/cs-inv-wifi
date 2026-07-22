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
#include "mbedtls/md5.h"
#include "esp_spiffs.h"
#include "offline_cache.h"
#include <stdio.h>

static const char* TAG = "ARM-OTA";

#define ARM_OTA_STREAM_BUF_SIZE  2048
#define ARM_OTA_END_QUIET_MS     100

static ArmOtaState       g_ota_state     = ARM_OTA_IDLE;
static ArmOtaCallback    g_ota_callback  = NULL;
static ArmOtaCancelCheck g_cancel_check  = NULL;
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
static uint8_t    g_stream_packet[ARM_OTA_PACKET_SIZE];
static size_t     g_stream_packet_len = 0;
static uint16_t   g_stream_packet_idx = UINT16_MAX;
static bool       g_waiting_end_ack = false;
static uint8_t    g_firmware_md5[ARM_OTA_MD5_SIZE];
static bool       g_firmware_md5_valid = false;
static FILE*      g_stage_file = NULL;
static size_t     g_stage_written = 0;
static size_t     g_stage_total = 0;
static bool       g_spiffs_ready = false;
static mbedtls_md5_context g_stage_md5;
static const volatile bool* g_abort_flag = NULL;

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

    if (!g_firmware_md5_valid) return false;
    memcpy(buf + pos, g_firmware_md5, ARM_OTA_MD5_SIZE);
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
        size_t required = (remaining > ARM_OTA_PACKET_SIZE) ? ARM_OTA_PACKET_SIZE : remaining;
        if (g_stream_packet_idx != packet_idx) {
            if (g_stream_buf_len < required) return false;
            memcpy(g_stream_packet, g_stream_buf, required);
            g_stream_buf_len -= required;
            if (g_stream_buf_len > 0) {
                memmove(g_stream_buf, g_stream_buf + required, g_stream_buf_len);
            }
            g_stream_packet_len = required;
            g_stream_packet_idx = packet_idx;
        }
        chunk = g_stream_packet_len;
        memcpy(buf + pos, g_stream_packet, chunk);
        pos += (int)chunk;
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

    if (!g_firmware_md5_valid) return false;
    memcpy(buf + pos, g_firmware_md5, ARM_OTA_MD5_SIZE);
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
    /* Drain delayed DATA ACKs while they are harmlessly ignored in VERIFYING. */
    if (now - g_state_enter_time < ARM_OTA_END_QUIET_MS) {
        uart_protocol_process();
        return;
    }
    if (now - g_last_send_time < ARM_OTA_SEND_INTERVAL) {
        return;
    }

    if (send_ota_end()) {
        g_last_send_time = now;
        g_waiting_end_ack = true;
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
        (void)packet_len;
        update_progress();
        set_state(ARM_OTA_WAIT_ACK, "Waiting for ACK");
    }
}

static bool transport_start_streaming(size_t total_size) {
    if (total_size == 0 || !g_firmware_md5_valid) {
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
    g_stream_packet_len = 0;
    g_stream_packet_idx = UINT16_MAX;
    g_waiting_end_ack = false;
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

bool arm_ota_set_expected_md5(const uint8_t md5[ARM_OTA_MD5_SIZE]) {
    if (!md5) return false;
    memcpy(g_firmware_md5, md5, sizeof(g_firmware_md5));
    g_firmware_md5_valid = true;
    return true;
}

static bool transport_feed(const uint8_t* data, size_t len) {
    if (!g_streaming_mode || !data || len == 0) {
        return false;
    }

    size_t copied = 0;
    while (copied < len) {
        if (g_cancel_check && g_cancel_check()) return false;
        if (g_abort_flag && *g_abort_flag) return false;
        if (g_ota_state == ARM_OTA_ERROR || g_ota_state == ARM_OTA_TIMEOUT) return false;
        size_t free_bytes = ARM_OTA_STREAM_BUF_SIZE - g_stream_buf_len;
        if (free_bytes == 0) {
            uart_protocol_process();
            arm_ota_process();
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        size_t n = len - copied;
        if (n > free_bytes) n = free_bytes;
        memcpy(g_stream_buf + g_stream_buf_len, data + copied, n);
        g_stream_buf_len += n;
        copied += n;
        uart_protocol_process();
        arm_ota_process();
    }

    return true;
}

static bool transport_end_streaming(void) {
    if (!g_streaming_mode) {
        return false;
    }

    int64_t deadline = get_time_ms() + 60000;
    while (g_current_packet < g_total_packets && get_time_ms() < deadline) {
        if (g_cancel_check && g_cancel_check()) return false;
        if (g_abort_flag && *g_abort_flag) return false;
        uart_protocol_process();
        if (g_current_packet >= g_total_packets) break;
        arm_ota_process();
        if (g_ota_state == ARM_OTA_ERROR || g_ota_state == ARM_OTA_TIMEOUT) return false;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (g_current_packet != g_total_packets || g_stream_buf_len != 0) return false;
    set_state(ARM_OTA_VERIFYING, "All packets acknowledged");
    return true;
}

static bool ensure_stage_fs(void) {
    if (g_spiffs_ready) return true;
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/cache",
        .partition_label = "cache",
        .max_files = 2,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to mount OTA cache: %s", esp_err_to_name(err));
        return false;
    }
    g_spiffs_ready = true;
    return true;
}

bool arm_ota_start_streaming(size_t total_size) {
    if (total_size == 0 || total_size > 256 * 1024 || g_stage_file || g_streaming_mode) {
        return false;
    }
    if (!ensure_stage_fs()) return false;
    if (!offline_cache_reserve_space(total_size + 16 * 1024)) {
        ESP_LOGE(TAG, "Insufficient cache space for ARM OTA staging");
        return false;
    }
    g_stage_file = fopen("/cache/arm_ota.bin", "wb");
    if (!g_stage_file) return false;
    g_stage_total = total_size;
    g_stage_written = 0;
    mbedtls_md5_init(&g_stage_md5);
    mbedtls_md5_starts(&g_stage_md5);
    ESP_LOGI(TAG, "Staging ARM firmware: %u bytes", (unsigned)total_size);
    return true;
}

bool arm_ota_feed(const uint8_t* data, size_t len) {
    if (!g_stage_file || !data || len == 0 || g_stage_written + len > g_stage_total) {
        return false;
    }
    if (fwrite(data, 1, len, g_stage_file) != len) {
        arm_ota_cancel();
        return false;
    }
    mbedtls_md5_update(&g_stage_md5, data, len);
    g_stage_written += len;
    return true;
}

bool arm_ota_end_streaming_cancelable(const volatile bool* cancel_flag) {
    if (!g_stage_file) return false;
    if (g_stage_written != g_stage_total) {
        arm_ota_cancel();
        return false;
    }
    mbedtls_md5_finish(&g_stage_md5, g_firmware_md5);
    mbedtls_md5_free(&g_stage_md5);
    g_firmware_md5_valid = true;
    fclose(g_stage_file);
    g_stage_file = NULL;

    g_abort_flag = cancel_flag;
    if (!transport_start_streaming(g_stage_total)) {
        g_abort_flag = NULL;
        remove("/cache/arm_ota.bin");
        return false;
    }
    FILE* input = fopen("/cache/arm_ota.bin", "rb");
    if (!input) {
        arm_ota_cancel();
        return false;
    }
    uint8_t chunk[1024];
    bool ok = true;
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), input)) > 0) {
        if (g_cancel_check && g_cancel_check()) {
            ok = false;
            break;
        }
        if (g_abort_flag && *g_abort_flag) {
            ok = false;
            break;
        }
        if (!transport_feed(chunk, n)) {
            ok = false;
            break;
        }
    }
    if (ferror(input)) ok = false;
    fclose(input);
    remove("/cache/arm_ota.bin");
    if (ok) ok = transport_end_streaming();
    if (!ok && cancel_flag && *cancel_flag) arm_ota_cancel();
    g_abort_flag = NULL;
    return ok;
}

bool arm_ota_end_streaming(void) {
    return arm_ota_end_streaming_cancelable(NULL);
}

bool arm_ota_is_streaming_mode(void) {
    return g_streaming_mode || g_stage_file != NULL;
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
    memset(g_firmware_md5, 0, sizeof(g_firmware_md5));
    g_firmware_md5_valid = false;

    ESP_LOGI(TAG, "Module initialized");
}

void arm_ota_set_callback(ArmOtaCallback cb) {
    g_ota_callback = cb;
}

void arm_ota_set_cancel_check(ArmOtaCancelCheck cb) {
    g_cancel_check = cb;
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
    if (mbedtls_md5(firmware, length, g_firmware_md5) != 0) {
        set_state(ARM_OTA_ERROR, "Failed to calculate firmware MD5");
        return false;
    }
    g_firmware_md5_valid = true;
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
    if (g_stage_file) {
        fclose(g_stage_file);
        g_stage_file = NULL;
        mbedtls_md5_free(&g_stage_md5);
        remove("/cache/arm_ota.bin");
        g_stage_written = 0;
        g_stage_total = 0;
    }
    if (g_ota_state == ARM_OTA_IDLE && !g_streaming_mode) {
        return;
    }
    ESP_LOGW(TAG, "Cancelled from state %d", g_ota_state);
    g_firmware = NULL;
    g_firmware_len = 0;
    g_progress = 0;
    g_streaming_mode = false;
    g_stream_buf_len = 0;
    g_stream_packet_len = 0;
    g_stream_packet_idx = UINT16_MAX;
    g_waiting_end_ack = false;
    set_state(ARM_OTA_IDLE, "OTA cancelled");
}

void arm_ota_handle_uart(uint8_t cmd, const uint8_t* data, uint16_t len) {
    switch (cmd) {
        case CMD_ARM_OTA_ACK: {
            ESP_LOGI(TAG, "ACK received, len=%u", len);

            if (g_ota_state == ARM_OTA_WAIT_READY && g_waiting_end_ack) {
                g_waiting_end_ack = false;
                g_progress = 100;
                set_state(ARM_OTA_DONE, "OTA completed successfully");
                g_firmware = NULL;
                g_firmware_len = 0;
                g_streaming_mode = false;
            }
            else if (g_ota_state == ARM_OTA_WAIT_READY) {
                g_current_packet = 0;
                g_retry_count = 0;
                set_state(ARM_OTA_SENDING, "Sending firmware data");
            }
            else if (g_ota_state == ARM_OTA_WAIT_ACK) {
                g_retry_count = 0;
                g_current_packet++;
                g_stream_sent = (size_t)g_current_packet * ARM_OTA_PACKET_SIZE;
                if (g_stream_sent > g_stream_total) g_stream_sent = g_stream_total;
                g_stream_packet_len = 0;
                g_stream_packet_idx = UINT16_MAX;

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
