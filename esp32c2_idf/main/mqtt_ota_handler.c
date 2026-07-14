/**
 * ================================================================
 *  MQTT OTA 处理实现 (ESP-IDF)
 * ================================================================
 */

#include "mqtt_ota_handler.h"
#include "mqtt_handler.h"
#include "arm_ota.h"
#include "led_status.h"
#include "uart_protocol.h"
#include "config.h"
#include "cs_protocol_defs.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "cJSON.h"
#include "mbedtls/sha256.h"

static const char* TAG = "MQTT-OTA";

// ==================== OTA 大小限制 ====================
#define OTA_ESP_MAX_SIZE  (2 * 1024 * 1024)
#define OTA_ARM_MAX_SIZE  (256 * 1024)

// ==================== OTA 下载缓冲区 ====================
#define OTA_BUFFER_SIZE   8192

// ==================== 任务通知位 ====================
#define OTA_NOTIFY_START  (1 << 0)

// ==================== 全局变量 ====================

static struct MqttOtaCommand g_current_ota_cmd;
static bool g_ota_cmd_received = false;
static int64_t g_ota_cmd_time = 0;
static char g_current_task_id[64] = "";
static char g_current_ota_target[8] = "";
static int g_ota_total_bytes = 0;
static int g_ota_downloaded_bytes = 0;
static int64_t g_ota_start_time = 0;

static uint8_t g_ota_buffer[OTA_BUFFER_SIZE];
static TaskHandle_t g_ota_task_handle = NULL;

// ==================== 前向声明 ====================
static bool parse_ota_command(const uint8_t* payload, unsigned int len, struct MqttOtaCommand* cmd);
static void handle_ota_start_command(const struct MqttOtaCommand* cmd);
static void handle_ota_cancel_command(const struct MqttOtaCommand* cmd);
static void handle_ota_query_command(const struct MqttOtaCommand* cmd);
static void send_ota_ack(const char* task_id, bool ack, const char* message);
static void start_firmware_download(const struct MqttOtaCommand* cmd);
static void ota_download_task(void* pvParameters);
static void handle_esp_ota_download(esp_http_client_handle_t http_client, int content_length,
                                    const struct MqttOtaCommand* cmd);
static void handle_arm_ota_download(esp_http_client_handle_t http_client, int content_length,
                                    const struct MqttOtaCommand* cmd);

// ==================== 初始化 ====================

void mqtt_ota_init(void) {
    ESP_LOGI(TAG, "模块已初始化");

    memset(&g_current_ota_cmd, 0, sizeof(g_current_ota_cmd));

    const char* client_id = mqtt_get_client_id();
    if (client_id) {
        mqtt_subscribe("ota/cmd", 1);
        ESP_LOGI(TAG, "已订阅: cs_inv/%s/ota/cmd", client_id);
    }

    xTaskCreate(ota_download_task, "ota_dl", 8192, NULL, 5, &g_ota_task_handle);
}

// ==================== JSON 解析 ====================

static void trim_str(char* str) {
    if (!str) return;
    char* start = str;
    while (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r') start++;
    if (start != str) memmove(str, start, strlen(start) + 1);
    int len = strlen(str);
    while (len > 0 && (str[len-1] == ' ' || str[len-1] == '\t' ||
           str[len-1] == '\n' || str[len-1] == '\r')) {
        str[--len] = '\0';
    }
}

static bool parse_ota_command(const uint8_t* payload, unsigned int len,
                               struct MqttOtaCommand* cmd) {
    char* json_str = malloc(len + 1);
    if (!json_str) return false;
    memcpy(json_str, payload, len);
    json_str[len] = '\0';

    cJSON* root = cJSON_Parse(json_str);
    free(json_str);

    if (!root) {
        ESP_LOGE(TAG, "JSON 解析失败");
        return false;
    }

    cJSON* j;

    j = cJSON_GetObjectItem(root, "action");
    strncpy(cmd->action, cJSON_IsString(j) ? j->valuestring : "", sizeof(cmd->action) - 1);

    j = cJSON_GetObjectItem(root, "target");
    strncpy(cmd->target, cJSON_IsString(j) ? j->valuestring : "", sizeof(cmd->target) - 1);

    j = cJSON_GetObjectItem(root, "firmware_id");
    strncpy(cmd->firmware_id, cJSON_IsString(j) ? j->valuestring : "", sizeof(cmd->firmware_id) - 1);

    j = cJSON_GetObjectItem(root, "url");
    strncpy(cmd->url, cJSON_IsString(j) ? j->valuestring : "", sizeof(cmd->url) - 1);

    j = cJSON_GetObjectItem(root, "version");
    strncpy(cmd->version, cJSON_IsString(j) ? j->valuestring : "", sizeof(cmd->version) - 1);

    j = cJSON_GetObjectItem(root, "sha256");
    strncpy(cmd->sha256, cJSON_IsString(j) ? j->valuestring : "", sizeof(cmd->sha256) - 1);

    j = cJSON_GetObjectItem(root, "signature");
    strncpy(cmd->signature, cJSON_IsString(j) ? j->valuestring : "", sizeof(cmd->signature) - 1);

    j = cJSON_GetObjectItem(root, "task_id");
    strncpy(cmd->task_id, cJSON_IsString(j) ? j->valuestring : "", sizeof(cmd->task_id) - 1);

    trim_str(cmd->url);

    j = cJSON_GetObjectItem(root, "security_version");
    cmd->security_version = cJSON_IsNumber(j) ? (uint32_t)j->valuedouble : 0;

    j = cJSON_GetObjectItem(root, "file_size");
    cmd->file_size = cJSON_IsNumber(j) ? (uint32_t)j->valuedouble : 0;

    j = cJSON_GetObjectItem(root, "timeout");
    cmd->timeout = cJSON_IsNumber(j) ? (uint16_t)j->valueint : 300;

    j = cJSON_GetObjectItem(root, "timestamp");
    cmd->timestamp = cJSON_IsNumber(j) ? (uint32_t)j->valuedouble : 0;

    cJSON_Delete(root);
    return true;
}

// ==================== 命令处理 ====================

bool mqtt_ota_handle_command(const char* topic, const uint8_t* payload, unsigned int len) {
    ESP_LOGI(TAG, "收到命令 (主题: %s, 长度: %d)", topic, len);
    ESP_LOGI(TAG, "Payload: %.*s", (int)len, payload);

    struct MqttOtaCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    if (!parse_ota_command(payload, len, &cmd)) {
        mqtt_ota_report_error("unknown", "JSON_ERROR", "命令格式错误");
        return false;
    }

    if (strlen(cmd.action) == 0) {
        mqtt_ota_report_error("unknown", PROTO_ERR_INVALID_ARGS, "缺少 action 字段");
        return false;
    }

    ESP_LOGI(TAG, "action: %s, 目标: %s, URL: %s", cmd.action, cmd.target, cmd.url);

    memcpy(&g_current_ota_cmd, &cmd, sizeof(cmd));

    if (strcmp(cmd.action, PROTO_OTA_ACTION_START) == 0) {
        handle_ota_start_command(&cmd);
    } else if (strcmp(cmd.action, PROTO_OTA_ACTION_CANCEL) == 0) {
        handle_ota_cancel_command(&cmd);
    } else if (strcmp(cmd.action, PROTO_OTA_ACTION_QUERY) == 0) {
        handle_ota_query_command(&cmd);
    } else {
        ESP_LOGW(TAG, "未知 action: %s", cmd.action);
        mqtt_ota_report_error(cmd.target, PROTO_ERR_NOT_SUPPORTED, "action 不支持");
    }
    return true;
}

// ==================== 命令处理函数 ====================

static void handle_ota_start_command(const struct MqttOtaCommand* cmd) {
    ESP_LOGI(TAG, "处理 START 命令");

    if (strlen(cmd->target) == 0 || strlen(cmd->url) == 0) {
        mqtt_ota_report_error(cmd->target, "INVALID_PARAMS", "缺少 target 或 url");
        return;
    }

    if (strcmp(cmd->target, "esp") != 0 && strcmp(cmd->target, "arm") != 0) {
        mqtt_ota_report_error(cmd->target, "INVALID_TARGET", "target 只能是 esp 或 arm");
        return;
    }

    if (mqtt_ota_is_busy()) {
        mqtt_ota_report_error(cmd->target, "OTA_BUSY", "已有 OTA 操作进行中");
        return;
    }

    strncpy(g_current_task_id, cmd->task_id, sizeof(g_current_task_id) - 1);
    strncpy(g_current_ota_target, cmd->target, sizeof(g_current_ota_target) - 1);
    g_ota_total_bytes = cmd->file_size;
    g_ota_downloaded_bytes = 0;
    g_ota_start_time = esp_timer_get_time() / 1000;
    g_ota_cmd_received = true;
    g_ota_cmd_time = esp_timer_get_time() / 1000;

    memcpy(&g_current_ota_cmd, cmd, sizeof(struct MqttOtaCommand));

    send_ota_ack(cmd->task_id, true, "OTA 命令已接收");

    // Ed25519 签名校验（当前仅 stub）
    // TODO: 集成 mbedtls Ed25519 签名校验
    if (cmd->signature[0] != '\0') {
        ESP_LOGW(TAG, "Ed25519 signature present but not verified (TODO)");
    }

    mqtt_ota_report_status(cmd->target, PROTO_OTA_STATE_ACCEPTED, 0,
                           PROTO_ERR_OK, "准备下载固件");

    if (g_ota_task_handle) {
        xTaskNotifyGive(g_ota_task_handle);
        ESP_LOGI(TAG, "已通知 OTA 下载任务");
    }
}

static void handle_ota_cancel_command(const struct MqttOtaCommand* cmd) {
    ESP_LOGI(TAG, "处理 CANCEL 命令");

    if (mqtt_ota_is_busy()) {
        ESP_LOGI(TAG, "取消 OTA 操作");
        mqtt_ota_report_status(cmd->target, PROTO_OTA_STATE_CANCELLED, 0,
                               PROTO_ERR_OK, "OTA 已取消");
        reset_ota_state();
    } else {
        ESP_LOGI(TAG, "当前无 OTA 操作，忽略 cancel");
        mqtt_ota_report_status(cmd->target, PROTO_OTA_STATE_CANCELLED, 0,
                               PROTO_ERR_OK, "无活跃 OTA");
    }
}

static void handle_ota_query_command(const struct MqttOtaCommand* cmd) {
    ESP_LOGI(TAG, "处理 QUERY 命令");

    if (mqtt_ota_is_busy()) {
        mqtt_ota_report_status(g_current_ota_target, PROTO_OTA_STATE_DOWNLOADING, 50,
                               PROTO_ERR_OK, "查询中");
    } else {
        mqtt_ota_report_status("esp", PROTO_OTA_STATE_ACCEPTED, 0,
                               PROTO_ERR_OK, "空闲");
    }
}

// ==================== 重置 OTA 状态 ====================

static void reset_ota_state(void) {
    g_ota_cmd_received = false;
    memset(g_current_task_id, 0, sizeof(g_current_task_id));
    memset(g_current_ota_target, 0, sizeof(g_current_ota_target));
    g_ota_total_bytes = 0;
    g_ota_downloaded_bytes = 0;
    ESP_LOGI(TAG, "状态已重置");
}

// ==================== 固件下载 ====================

static void start_firmware_download(const struct MqttOtaCommand* cmd) {
    ESP_LOGI(TAG, "启动固件下载: %s", cmd->url);

    esp_http_client_config_t config = {
        .url = cmd->url,
        .timeout_ms = cmd->timeout * 1000,
        .buffer_size = 4096,
        .buffer_size_tx = 512,
    };

    esp_http_client_handle_t http_client = esp_http_client_init(&config);
    if (!http_client) {
        ESP_LOGE(TAG, "无法初始化 HTTP 客户端");
        mqtt_ota_report_error(cmd->target, "HTTP_INIT_FAILED", "HTTP 客户端初始化失败");
        reset_ota_state();
        return;
    }

    esp_http_client_set_header(http_client, "User-Agent", "ESP32C2-OTA/1.0");
    esp_http_client_set_header(http_client, "Accept-Encoding", "identity");

    esp_err_t err = esp_http_client_open(http_client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 连接失败: %s", esp_err_to_name(err));
        mqtt_ota_report_error(cmd->target, "HTTP_ERROR", "HTTP 连接失败");
        esp_http_client_cleanup(http_client);
        reset_ota_state();
        return;
    }

    int content_length = esp_http_client_fetch_headers(http_client);
    int status_code = esp_http_client_get_status_code(http_client);

    ESP_LOGI(TAG, "HTTP 状态码: %d, 响应大小: %d 字节", status_code, content_length);

    if (status_code != 200 && status_code != 302) {
        ESP_LOGE(TAG, "HTTP 请求失败: %d", status_code);
        char err_msg[32];
        snprintf(err_msg, sizeof(err_msg), "HTTP %d", status_code);
        mqtt_ota_report_error(cmd->target, "HTTP_ERROR", err_msg);
        esp_http_client_close(http_client);
        esp_http_client_cleanup(http_client);
        reset_ota_state();
        return;
    }

    if (content_length <= 0) {
        ESP_LOGE(TAG, "无效的内容长度");
        mqtt_ota_report_error(cmd->target, "INVALID_SIZE", "无效的内容长度");
        esp_http_client_close(http_client);
        esp_http_client_cleanup(http_client);
        reset_ota_state();
        return;
    }

    if (strcmp(cmd->target, "esp") == 0 && content_length > OTA_ESP_MAX_SIZE) {
        ESP_LOGE(TAG, "文件过大: %d > %d", content_length, OTA_ESP_MAX_SIZE);
        mqtt_ota_report_error(cmd->target, "FILE_TOO_LARGE", "ESP 固件超过 2MB");
        esp_http_client_close(http_client);
        esp_http_client_cleanup(http_client);
        reset_ota_state();
        return;
    }

    if (strcmp(cmd->target, "arm") == 0 && content_length > OTA_ARM_MAX_SIZE) {
        ESP_LOGE(TAG, "文件过大: %d > %d", content_length, OTA_ARM_MAX_SIZE);
        mqtt_ota_report_error(cmd->target, "FILE_TOO_LARGE", "ARM 固件超过 256KB");
        esp_http_client_close(http_client);
        esp_http_client_cleanup(http_client);
        reset_ota_state();
        return;
    }

    if (strcmp(cmd->target, "esp") == 0) {
        handle_esp_ota_download(http_client, content_length, cmd);
    } else if (strcmp(cmd->target, "arm") == 0) {
        handle_arm_ota_download(http_client, content_length, cmd);
    }

    esp_http_client_close(http_client);
    esp_http_client_cleanup(http_client);
}

// ==================== OTA 下载任务 ====================

static void ota_download_task(void* pvParameters) {
    ESP_LOGI(TAG, "OTA 下载任务已启动");
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ESP_LOGI(TAG, "收到下载通知, target=%s", g_current_ota_target);
        vTaskDelay(pdMS_TO_TICKS(100));
        start_firmware_download(&g_current_ota_cmd);
    }
}

// ==================== ESP OTA 下载处理 ====================

static void handle_esp_ota_download(esp_http_client_handle_t http_client, int content_length,
                                    const struct MqttOtaCommand* cmd) {

    ESP_LOGI(TAG, "[ESP] 开始流式下载和写入");

    const esp_partition_t* update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE(TAG, "[ESP] 无法获取 OTA 分区");
        mqtt_ota_report_error("esp", "UPDATE_BEGIN_FAILED", "无法获取 OTA 分区");
        reset_ota_state();
        return;
    }

    esp_ota_handle_t ota_handle;
    esp_err_t ota_err = esp_ota_begin(update_partition, content_length, &ota_handle);
    if (ota_err != ESP_OK) {
        ESP_LOGE(TAG, "[ESP] esp_ota_begin() 失败: %s", esp_err_to_name(ota_err));
        mqtt_ota_report_error("esp", "UPDATE_BEGIN_FAILED", "OTA 初始化失败");
        reset_ota_state();
        return;
    }

    // 初始化 SHA-256 计算
    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts_ret(&sha_ctx, 0);

    int bytes_written = 0;
    int prev_progress = 0;
    int64_t last_update = esp_timer_get_time() / 1000;
    int64_t download_start = esp_timer_get_time() / 1000;

    led1_set(LED_FAST_BLINK);

    while (bytes_written < content_length) {
        if ((esp_timer_get_time() / 1000) - download_start > 300000) {
            ESP_LOGE(TAG, "[ESP] 下载超时");
            esp_ota_abort(ota_handle);
            mqtt_ota_report_error("esp", "DOWNLOAD_TIMEOUT", "下载超时（300s）");
            led1_set(LED_OFF);
            reset_ota_state();
            return;
        }

        uart_protocol_process();

        int to_read = content_length - bytes_written;
        if (to_read > OTA_BUFFER_SIZE) to_read = OTA_BUFFER_SIZE;
        int bytes_read = esp_http_client_read(http_client, (char*)g_ota_buffer, to_read);

        if (bytes_read > 0) {
            ota_err = esp_ota_write(ota_handle, g_ota_buffer, bytes_read);
            if (ota_err != ESP_OK) {
                ESP_LOGE(TAG, "[ESP] 写入失败: %s", esp_err_to_name(ota_err));
                esp_ota_abort(ota_handle);
                mqtt_ota_report_error("esp", "WRITE_FAILED", "Flash 写入失败");
                led1_set(LED_OFF);
                reset_ota_state();
                return;
            }

            // SHA-256 更新
            mbedtls_sha256_update_ret(&sha_ctx, g_ota_buffer, bytes_read);

            bytes_written += bytes_read;
            g_ota_downloaded_bytes = bytes_written;

            if ((esp_timer_get_time() / 1000) - last_update > 1000) {
                int progress = (bytes_written * 100) / content_length;
                if (progress != prev_progress) {
                    prev_progress = progress;
                    int elapsed = (int)((esp_timer_get_time() / 1000 - download_start) / 1000);
                    int speed_kbps = (bytes_written / 1024) / (elapsed + 1);

                    ESP_LOGI(TAG, "[ESP] 进度: %d%% (%d/%d), 速度: %d KB/s",
                             progress, bytes_written, content_length, speed_kbps);

                    char detail[32];
                    snprintf(detail, sizeof(detail), "下载中: %d%%", progress);
                    mqtt_ota_report_status("esp", PROTO_OTA_STATE_DOWNLOADING, progress,
                                           PROTO_ERR_OK, detail);
                }
                last_update = esp_timer_get_time() / 1000;
            }
        } else if (bytes_read < 0) {
            ESP_LOGE(TAG, "[ESP] HTTP 读取错误");
            esp_ota_abort(ota_handle);
            mqtt_ota_report_error("esp", "READ_FAILED", "HTTP 读取失败");
            led1_set(LED_OFF);
            reset_ota_state();
            return;
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    ESP_LOGI(TAG, "[ESP] 下载完成, 共 %d 字节", bytes_written);
    mqtt_ota_report_status("esp", PROTO_OTA_STATE_VERIFYING, 100,
                           PROTO_ERR_OK, "校验中...");

    led1_set(LED_SOLID);

    // SHA-256 校验
    if (cmd->sha256[0] != '\0') {
        uint8_t hash[32];
        mbedtls_sha256_finish_ret(&sha_ctx, hash);
        mbedtls_sha256_free(&sha_ctx);

        char hex[65];
        for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", hash[i]);

        if (strcmp(hex, cmd->sha256) != 0) {
            ESP_LOGE(TAG, "[ESP] SHA-256 校验失败: expected=%s, got=%s", cmd->sha256, hex);
            esp_ota_abort(ota_handle);
            mqtt_ota_report_error("esp", PROTO_ERR_EXEC_FAILED, "SHA-256 校验失败");
            led1_set(LED_OFF);
            reset_ota_state();
            return;
        }
        ESP_LOGI(TAG, "[ESP] SHA-256 校验通过");
    } else {
        mbedtls_sha256_free(&sha_ctx);
        ESP_LOGI(TAG, "[ESP] 未提供 SHA-256，跳过校验");
    }

    ota_err = esp_ota_end(ota_handle);
    if (ota_err != ESP_OK) {
        ESP_LOGE(TAG, "[ESP] 校验失败: %s", esp_err_to_name(ota_err));
        mqtt_ota_report_error("esp", "VERIFY_FAILED", "固件校验失败");
        led1_set(LED_OFF);
        reset_ota_state();
        return;
    }

    esp_ota_set_boot_partition(update_partition);

    ESP_LOGI(TAG, "[ESP] 升级成功，设备即将重启");
    mqtt_ota_report_status("esp", PROTO_OTA_STATE_REBOOTING, 100,
                           PROTO_ERR_OK, "升级成功，重启中");
    led1_set(LED_OFF);

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

// ==================== ARM OTA 下载处理 ====================

static void handle_arm_ota_download(esp_http_client_handle_t http_client, int content_length,
                                    const struct MqttOtaCommand* cmd) {

    ESP_LOGI(TAG, "[ARM] 开始流式下载和转发");

    if (!arm_ota_start_streaming(content_length)) {
        ESP_LOGE(TAG, "[ARM] arm_ota_start_streaming() 失败");
        mqtt_ota_report_error("arm", "ARM_OTA_START", "ARM OTA 初始化失败");
        reset_ota_state();
        return;
    }

    int bytes_sent = 0;
    int prev_progress = 0;
    int64_t last_update = esp_timer_get_time() / 1000;
    int64_t download_start = esp_timer_get_time() / 1000;

    led1_set(LED_FAST_BLINK);

    while (bytes_sent < content_length) {
        if ((esp_timer_get_time() / 1000) - download_start > 600000) {
            ESP_LOGE(TAG, "[ARM] 总超时");
            arm_ota_cancel();
            mqtt_ota_report_error("arm", "TIMEOUT", "传输超时");
            led1_set(LED_OFF);
            reset_ota_state();
            return;
        }

        uart_protocol_process();

        int to_read = content_length - bytes_sent;
        if (to_read > 1024) to_read = 1024;
        int bytes_read = esp_http_client_read(http_client, (char*)g_ota_buffer, to_read);

        if (bytes_read > 0) {
            if (!arm_ota_feed(g_ota_buffer, bytes_read)) {
                ESP_LOGE(TAG, "[ARM] arm_ota_feed() 失败");
                mqtt_ota_report_error("arm", "FEED_FAILED", "ARM 缓冲失败");
                led1_set(LED_OFF);
                reset_ota_state();
                return;
            }

            bytes_sent += bytes_read;
            g_ota_downloaded_bytes = bytes_sent;

            if ((esp_timer_get_time() / 1000) - last_update > 1000) {
                int progress = (bytes_sent * 100) / content_length;
                if (progress != prev_progress) {
                    prev_progress = progress;
                    int elapsed = (int)((esp_timer_get_time() / 1000 - download_start) / 1000);
                    int speed_kbps = (bytes_sent / 1024) / (elapsed + 1);

                    ESP_LOGI(TAG, "[ARM] 进度: %d%% (%d/%d), 速度: %d KB/s",
                             progress, bytes_sent, content_length, speed_kbps);

                    char detail[32];
                    snprintf(detail, sizeof(detail), "下载中: %d%%", progress);
                    mqtt_ota_report_status("arm", PROTO_OTA_STATE_DOWNLOADING, progress,
                                           PROTO_ERR_OK, detail);
                }
                last_update = esp_timer_get_time() / 1000;
            }
        } else if (bytes_read < 0) {
            ESP_LOGE(TAG, "[ARM] HTTP 读取错误");
            arm_ota_cancel();
            mqtt_ota_report_error("arm", "READ_FAILED", "HTTP 读取失败");
            led1_set(LED_OFF);
            reset_ota_state();
            return;
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    ESP_LOGI(TAG, "[ARM] 下载完成, 共 %d 字节", bytes_sent);

    if (!arm_ota_end_streaming()) {
        ESP_LOGE(TAG, "[ARM] arm_ota_end_streaming() 失败");
        mqtt_ota_report_error("arm", "END_FAILED", "ARM 结束失败");
        led1_set(LED_OFF);
        reset_ota_state();
        return;
    }

    mqtt_ota_report_status("arm", PROTO_OTA_STATE_VERIFYING, 100,
                           PROTO_ERR_OK, "等待 ARM 校验");
    led1_set(LED_SOLID);

    ESP_LOGI(TAG, "[ARM] 固件已转发，等待 ARM 校验");
}

// ==================== 状态上报 ====================

void mqtt_ota_report_status(const char* target, const char* state, int progress,
                            const char* code, const char* detail) {
    cJSON* root = cJSON_CreateObject();
    if (!root) return;

    cJSON_AddStringToObject(root, "target", target);
    const char* ver = g_current_ota_cmd.version[0] ? g_current_ota_cmd.version : FIRMWARE_VERSION;
    cJSON_AddStringToObject(root, "version", ver);
    cJSON_AddStringToObject(root, "state", state);
    cJSON_AddNumberToObject(root, "progress", progress);
    cJSON_AddStringToObject(root, "code", code ? code : PROTO_ERR_OK);
    cJSON_AddStringToObject(root, "message", detail ? detail : "");

    char* payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (payload) {
        mqtt_publish(PROTO_TOPIC_OTA_STATUS, payload, strlen(payload), 0, 0);
        ESP_LOGI(TAG, "上报状态: %s", payload);
        free(payload);
    }
}

void mqtt_ota_report_error(const char* target, const char* error_code,
                           const char* error_message) {
    cJSON* root = cJSON_CreateObject();
    if (!root) return;

    cJSON_AddStringToObject(root, "target", target);
    const char* ver = g_current_ota_cmd.version[0] ? g_current_ota_cmd.version : FIRMWARE_VERSION;
    cJSON_AddStringToObject(root, "version", ver);
    cJSON_AddStringToObject(root, "state", PROTO_OTA_STATE_FAILED);
    cJSON_AddNumberToObject(root, "progress", 0);
    cJSON_AddStringToObject(root, "code", error_code ? error_code : PROTO_ERR_EXEC_FAILED);
    cJSON_AddStringToObject(root, "message", error_message ? error_message : "");

    char* payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (payload) {
        mqtt_publish(PROTO_TOPIC_OTA_STATUS, payload, strlen(payload), 1, 0);
        ESP_LOGI(TAG, "上报错误: %s", payload);
        free(payload);
    }
}

static void send_ota_ack(const char* task_id, bool ack, const char* message) {
    cJSON* root = cJSON_CreateObject();
    if (!root) return;

    cJSON_AddStringToObject(root, "task_id", task_id ? task_id : "");
    cJSON_AddStringToObject(root, "cmd", "ota");
    cJSON_AddStringToObject(root, "stage", ack ? PROTO_CMD_STAGE_ACK : PROTO_CMD_STAGE_DONE);
    cJSON_AddBoolToObject(root, "success", ack);
    cJSON_AddStringToObject(root, "code", ack ? PROTO_ERR_OK : PROTO_ERR_EXEC_FAILED);
    cJSON_AddStringToObject(root, "message", message ? message : "");
    cJSON_AddItemToObject(root, "result", cJSON_CreateArray());

    char* payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (payload) {
        mqtt_publish(PROTO_TOPIC_CMD_RESP, payload, strlen(payload), 1, 0);
        ESP_LOGI(TAG, "发送 ACK: %s", payload);
        free(payload);
    }
}

// ==================== 状态查询 ====================

void mqtt_ota_process(void) {
    uart_protocol_process();
    arm_ota_process();
}

bool mqtt_ota_is_busy(void) {
    return g_ota_cmd_received &&
           ((esp_timer_get_time() / 1000) - g_ota_cmd_time < 600000);
}

const char* mqtt_ota_get_task_id(void) {
    return g_current_task_id;
}
