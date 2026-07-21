/**
 * ================================================================
 *  MQTT OTA 处理实现
 * ================================================================
 */

#include "mqtt_ota_handler.h"
#include "mqtt_handler.h"
#include "ota_manager.h"
#include "arm_ota.h"
#include "led_status.h"
#include "uart_protocol.h"
#include "ota_result.h"
#include "ota_signature.h"
#include "cs_protocol_defs.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <string.h>
#include "mbedtls/sha256.h"

// ==================== OTA 大小限制 ====================
#define OTA_ESP_MAX_SIZE  0x1A0000               // ESP: OTA slot size
#define OTA_ARM_MAX_SIZE  (256 * 1024)       // ARM: 256 KB

// ==================== 工具宏 ====================
#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif

// ==================== 全局变量 ====================

static struct MqttOtaCommand g_current_ota_cmd = {0};
static bool g_ota_cmd_received = false;
static unsigned long g_ota_cmd_time = 0;
static String g_current_task_id = "";
static String g_current_ota_target = "";
static int g_ota_total_bytes = 0;
static int g_ota_downloaded_bytes = 0;
static unsigned long g_ota_start_time = 0;
static bool g_download_pending = false;
#define OTA_RECENT_TASK_COUNT 8
static String g_recent_ota_tasks[OTA_RECENT_TASK_COUNT];
static uint8_t g_recent_ota_next = 0;
static String g_persisted_ota_task;
static String g_persisted_ota_target;
static String g_persisted_ota_status;
static volatile bool g_cancel_requested = false;
static String g_cancel_task_id;

// OTA 下载缓冲区（静态分配，避免栈溢出）
static uint8_t g_ota_buffer[4096];

// ==================== 前向声明 ====================
static bool parse_ota_command(const uint8_t* payload, unsigned int len, struct MqttOtaCommand* cmd);
static void handle_ota_start_command(const struct MqttOtaCommand* cmd);
static void handle_ota_cancel_command(const struct MqttOtaCommand* cmd);
static void handle_ota_query_command(const struct MqttOtaCommand* cmd);
static void send_ota_ack(const char* task_id, bool ack, const char* message);
static void send_ota_reject(const char* task_id, const char* code, const char* message);
static void send_persisted_ota_result(const char* task_id);
static void send_ota_task_result(const char* task_id, const char* target,
                                 const char* state, bool success, const char* message);
static void finish_cancelled_ota(const char* target);
static void start_firmware_download(const struct MqttOtaCommand* cmd);
static void handle_esp_ota_download(HTTPClient* http, int content_length, const struct MqttOtaCommand* cmd);
static void handle_arm_ota_download(HTTPClient* http, int content_length, const struct MqttOtaCommand* cmd);
static void reset_ota_state();
static bool ota_task_seen(const char* task_id);
static void remember_ota_task(const char* task_id);

static bool ota_task_seen(const char* task_id) {
  for (uint8_t i = 0; i < OTA_RECENT_TASK_COUNT; ++i) {
    if (g_recent_ota_tasks[i] == task_id) return true;
  }
  return false;
}

static void remember_ota_task(const char* task_id) {
  g_recent_ota_tasks[g_recent_ota_next] = task_id;
  g_recent_ota_next = (g_recent_ota_next + 1) % OTA_RECENT_TASK_COUNT;
}

// ==================== 初始化 ====================

void mqtt_ota_init() {
  Serial.println("[MQTT-OTA] 模块已初始化");

  OtaResult saved = {};
  if (ota_result_load(&saved) && saved.task_id[0]) {
    g_persisted_ota_task = saved.task_id;
    g_persisted_ota_target = saved.target;
    g_persisted_ota_status = saved.status;
  }

  // 订阅 OTA 命令主题
  const char* client_id = mqtt_get_client_id();
  if (client_id) {
    char topic[128];
    snprintf(topic, sizeof(topic), "cs_inv/%s/ota/cmd", client_id);
    mqtt_subscribe("ota/cmd", 1);  // 使用子主题订阅
    Serial.printf("[MQTT-OTA] 已订阅: %s\n", topic);
  }
}

// ==================== JSON 解析 ====================

// 去除字符串两端的空格
static void trim_str(char* str) {
  if (!str) return;
  // 去除开头空格
  char* start = str;
  while (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r') start++;
  if (start != str) memmove(str, start, strlen(start) + 1);
  // 去除结尾空格
  int len = strlen(str);
  while (len > 0 && (str[len-1] == ' ' || str[len-1] == '\t' || str[len-1] == '\n' || str[len-1] == '\r')) {
    str[--len] = '\0';
  }
}

static bool parse_ota_command(const uint8_t* payload, unsigned int len,
                               struct MqttOtaCommand* cmd) {
  if (!payload || !cmd || len == 0 || len >= 1024) return false;
  // 确保 payload 以 null 结尾
  static char json_buf[1024];
  memset(json_buf, 0, sizeof(json_buf));
  strncpy(json_buf, (const char*)payload, min(len, sizeof(json_buf) - 1));

  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, json_buf);

  if (error) {
    Serial.printf("[MQTT-OTA] JSON 解析失败: %s\n", error.c_str());
    return false;
  }

  if (!doc["t"].is<uint32_t>() && !doc["timestamp"].is<uint32_t>()) {
    Serial.println("[MQTT-OTA] Missing numeric t");
    return false;
  }

  // 提取字段
  cmd->protocol_version = doc["v"] | 0;
  strncpy(cmd->action, doc["action"] | "", sizeof(cmd->action) - 1);
  strncpy(cmd->target, doc["target"] | "", sizeof(cmd->target) - 1);
  strncpy(cmd->firmware_id, doc["firmware_id"] | "", sizeof(cmd->firmware_id) - 1);
  strncpy(cmd->url, doc["url"] | "", sizeof(cmd->url) - 1);
  strncpy(cmd->version, doc["version"] | "", sizeof(cmd->version) - 1);
  strncpy(cmd->sha256, doc["sha256"] | "", sizeof(cmd->sha256) - 1);
  strncpy(cmd->md5, doc["md5"] | "", sizeof(cmd->md5) - 1);
  strncpy(cmd->signature, doc["signature"] | "", sizeof(cmd->signature) - 1);
  strncpy(cmd->task_id, doc["task_id"] | "", sizeof(cmd->task_id) - 1);

  // 去除 URL 两端的空格（服务器可能发送带空格的 URL）
  trim_str(cmd->url);

  cmd->security_version = doc["security_version"] | (uint32_t)0;
  cmd->file_size = doc["size"] | 0;
  cmd->timeout = doc["timeout"] | 300;
  cmd->timestamp = doc["t"] | (uint32_t)0;

  return true;
}

// ==================== 命令处理 ====================

bool mqtt_ota_handle_command(const char* topic, const uint8_t* payload, unsigned int len) {

  Serial.printf("[MQTT-OTA] 收到命令 (主题: %s, 长度: %d)\n", topic, len);
  Serial.printf("[MQTT-OTA] Payload: %.*s\n", (int)len, payload);

  // 解析 JSON
  struct MqttOtaCommand cmd = {0};
  if (!parse_ota_command(payload, len, &cmd)) {
    send_ota_ack("", false, "Invalid OTA command JSON");
    return false;
  }

  // 验证必要字段
  if (strlen(cmd.action) == 0 || strlen(cmd.task_id) == 0) {
    send_ota_ack(cmd.task_id, false, "action and task_id are required");
    return false;
  }

  time_t now = time(nullptr);
  if (now > 0 && cmd.timestamp > 0 &&
      (now - (time_t)cmd.timestamp) > PROTO_CMD_EXPIRE_S) {
    send_ota_ack(cmd.task_id, false, "OTA command expired");
    return true;
  }

  if (g_persisted_ota_task == cmd.task_id) {
    send_persisted_ota_result(cmd.task_id);
    return true;
  }
  if (ota_task_seen(cmd.task_id) && strcmp(cmd.action, PROTO_OTA_ACTION_CANCEL) != 0) {
    send_ota_ack(cmd.task_id, true, "Duplicate ignored");
    return true;
  }

  Serial.printf("[MQTT-OTA] action: %s, 目标: %s, URL: %s\n",
    cmd.action, cmd.target, cmd.url);

  // 根据命令类型处理
  if (strcmp(cmd.action, PROTO_OTA_ACTION_START) == 0) {
    handle_ota_start_command(&cmd);
  } else if (strcmp(cmd.action, PROTO_OTA_ACTION_CANCEL) == 0) {
    handle_ota_cancel_command(&cmd);
  } else if (strcmp(cmd.action, PROTO_OTA_ACTION_QUERY) == 0) {
    handle_ota_query_command(&cmd);
  } else {
    Serial.printf("[MQTT-OTA] 未知 action: %s\n", cmd.action);
    send_ota_ack(cmd.task_id, false, "Unsupported OTA action");
  }
  return true;  // OTA 命令已处理（即使处理失败，也不应转发给 ARM）
}

// ==================== 命令处理函数 ====================

static void handle_ota_start_command(const struct MqttOtaCommand* cmd) {

  Serial.printf("[MQTT-OTA] 处理 START 命令\n");

  time_t now = time(nullptr);
  if (cmd->protocol_version != 1 || cmd->timestamp == 0 ||
      now <= 0 || llabs((long long)now - cmd->timestamp) > PROTO_CMD_EXPIRE_S ||
      !cmd->task_id[0] || !cmd->target[0] || !cmd->url[0] || !cmd->version[0] ||
      cmd->file_size == 0 || cmd->security_version == 0 || !cmd->signature[0]) {
    send_ota_ack(cmd->task_id, false, "Invalid or incomplete V1 OTA START");
    return;
  }

  if (strcmp(cmd->target, "esp") != 0 && strcmp(cmd->target, "arm") != 0) {
    send_ota_reject(cmd->task_id, PROTO_ERR_NOT_SUPPORTED, "Unsupported OTA target");
    return;
  }

  if (strncmp(cmd->url, "https://", 8) != 0) {
    send_ota_ack(cmd->task_id, false, "HTTPS URL required");
    return;
  }

  if (strlen(cmd->sha256) != 64 ||
      strspn(cmd->sha256, "0123456789abcdef") != 64) {
    send_ota_ack(cmd->task_id, false, "Valid SHA-256 required");
    return;
  }
  if (cmd->md5[0] &&
      (strlen(cmd->md5) != 32 || strspn(cmd->md5, "0123456789abcdefABCDEF") != 32)) {
    send_ota_ack(cmd->task_id, false, "Invalid optional MD5");
    return;
  }

  uint32_t stored_security_version = ota_security_version_get();
  if (cmd->security_version <= stored_security_version) {
    send_ota_ack(cmd->task_id, false, "Security version rollback rejected");
    return;
  }

  if (!ota_verify_release_signature(
          cmd->target, cmd->version, cmd->file_size, cmd->sha256,
          cmd->security_version, cmd->signature)) {
    send_ota_ack(cmd->task_id, false, "Ed25519 signature verification failed");
    return;
  }

  // 检查是否已有 OTA 在进行
  if (mqtt_ota_is_busy()) {
    send_ota_ack(cmd->task_id, false, "OTA busy");
    return;
  }

  remember_ota_task(cmd->task_id);

  // 保存任务信息
  g_current_task_id = cmd->task_id;
  g_current_ota_target = cmd->target;
  g_ota_total_bytes = cmd->file_size;
  g_ota_downloaded_bytes = 0;
  g_ota_start_time = millis();
  g_ota_cmd_received = true;
  g_ota_cmd_time = millis();

  memcpy(&g_current_ota_cmd, cmd, sizeof(struct MqttOtaCommand));

  // 发送应答
  send_ota_ack(cmd->task_id, true, "OTA 命令已接收");

  // 立即上报状态：已接受
  mqtt_ota_report_status(cmd->target, PROTO_OTA_STATE_ACCEPTED, 0,
                         PROTO_ERR_OK, "准备下载固件");

  // 不在 MQTT 回调中直接下载，由 mqtt_ota_process() 在主循环中触发
  g_download_pending = true;
  Serial.println("[MQTT-OTA] 下载将在主循环中启动");
}

static void handle_ota_cancel_command(const struct MqttOtaCommand* cmd) {
  Serial.printf("[MQTT-OTA] 处理 CANCEL 命令\n");
  remember_ota_task(cmd->task_id);

  if (mqtt_ota_is_busy()) {
    if (g_current_task_id != cmd->task_id) {
      send_ota_ack(cmd->task_id, false, "Cancel task_id does not match active OTA");
      return;
    }
    g_cancel_task_id = cmd->task_id;
    g_cancel_requested = true;
    send_ota_ack(cmd->task_id, true, "Cancel requested");
  } else {
    send_ota_task_result(cmd->task_id, cmd->target, PROTO_OTA_STATE_CANCELLED,
                         true, "No active OTA");
  }
}

static void handle_ota_query_command(const struct MqttOtaCommand* cmd) {
  Serial.printf("[MQTT-OTA] 处理 QUERY 命令\n");
  remember_ota_task(cmd->task_id);

  // 上报当前 OTA 状态
  if (mqtt_ota_is_busy()) {
    String detail = "active_task=" + g_current_task_id;
    send_ota_task_result(cmd->task_id, g_current_ota_target.c_str(),
                         PROTO_OTA_STATE_DOWNLOADING, true, detail.c_str());
  } else {
    send_ota_task_result(cmd->task_id, cmd->target, "idle", true, "No active OTA");
  }
}

// ==================== 重置 OTA 状态 ====================

static void reset_ota_state() {
  g_ota_cmd_received = false;
  g_download_pending = false;
  g_cancel_requested = false;
  g_cancel_task_id = "";
  g_current_task_id = "";
  g_current_ota_target = "";
  g_ota_total_bytes = 0;
  g_ota_downloaded_bytes = 0;
  Serial.println("[MQTT-OTA] 状态已重置");
}

static void finish_cancelled_ota(const char* target) {
  String active_task = g_current_task_id;
  String cancel_task = g_cancel_task_id;
  mqtt_ota_report_status(target, PROTO_OTA_STATE_CANCELLED, 0,
                         PROTO_ERR_OK, "OTA cancelled");
  ota_task_result_save(active_task.c_str(), target, "cancelled",
                       g_current_ota_cmd.version, "OTA cancelled");
  g_persisted_ota_task = active_task;
  g_persisted_ota_target = target;
  g_persisted_ota_status = "cancelled";
  send_ota_task_result(cancel_task.c_str(), target, PROTO_OTA_STATE_CANCELLED,
                       true, "Active OTA cancelled");
  reset_ota_state();
}

// ==================== 固件下载 ====================

static void start_firmware_download(const struct MqttOtaCommand* cmd) {

  Serial.printf("[MQTT-OTA] 启动固件下载: %s\n", cmd->url);

  // 创建 HTTP 客户端
  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(cmd->timeout * 1000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  // 开始连接
  if (!http.begin(cmd->url)) {
    Serial.println("[MQTT-OTA] 无法初始化 HTTP 客户端");
    mqtt_ota_report_error(cmd->target, "HTTP_INIT_FAILED", "HTTP 客户端初始化失败");
    reset_ota_state();
    return;
  }

  // 设置请求头
  http.addHeader("User-Agent", "ESP32-OTA/1.0");
  http.addHeader("Accept-Encoding", "identity");

  // 发送 GET 请求
  int http_code = http.GET();

  Serial.printf("[MQTT-OTA] HTTP 状态码: %d\n", http_code);

  if (http_code != HTTP_CODE_OK && http_code != HTTP_CODE_FOUND) {
    Serial.printf("[MQTT-OTA] HTTP 请求失败: %d\n", http_code);
    mqtt_ota_report_error(cmd->target, "HTTP_ERROR",
      String("HTTP " + String(http_code)).c_str());
    http.end();
    reset_ota_state();
    return;
  }

  // 获取内容长度
  int content_length = http.getSize();
  Serial.printf("[MQTT-OTA] 响应大小: %d 字节\n", content_length);

  if (content_length <= 0 || (uint32_t)content_length != cmd->file_size) {
    Serial.println("[MQTT-OTA] Content-Length 与声明 size 不一致");
    mqtt_ota_report_error(cmd->target, "INVALID_SIZE",
                          "Content-Length does not match declared size");
    http.end();
    reset_ota_state();
    return;
  }

  // 检查文件大小限制
  if (strcmp(cmd->target, "esp") == 0 && content_length > OTA_ESP_MAX_SIZE) {
    Serial.printf("[MQTT-OTA] 文件过大: %d > %d\n", content_length, OTA_ESP_MAX_SIZE);
    mqtt_ota_report_error(cmd->target, "FILE_TOO_LARGE", "ESP 固件超过 1MB");
    http.end();
    reset_ota_state();
    return;
  }

  if (strcmp(cmd->target, "arm") == 0 && content_length > OTA_ARM_MAX_SIZE) {
    Serial.printf("[MQTT-OTA] 文件过大: %d > %d\n", content_length, OTA_ARM_MAX_SIZE);
    mqtt_ota_report_error(cmd->target, "FILE_TOO_LARGE", "ARM 固件超过 256KB");
    http.end();
    reset_ota_state();
    return;
  }

  // 处理 ESP OTA
  if (strcmp(cmd->target, "esp") == 0) {
    handle_esp_ota_download(&http, content_length, cmd);
  }
  // 处理 ARM OTA
  else if (strcmp(cmd->target, "arm") == 0) {
    handle_arm_ota_download(&http, content_length, cmd);
  }

  http.end();
}

// ==================== ESP OTA 下载处理 ====================

static void handle_esp_ota_download(HTTPClient* http, int content_length,
                                     const struct MqttOtaCommand* cmd) {

  Serial.println("[MQTT-OTA-ESP] 开始流式下载和写入");

  // 启动 Update
  if (!Update.begin(content_length)) {
    Serial.println("[MQTT-OTA-ESP] Update.begin() 失败");
    mqtt_ota_report_error("esp", "UPDATE_BEGIN_FAILED", "Update 初始化失败");
    reset_ota_state();
    return;
  }

  // 初始化 SHA-256 计算
  mbedtls_sha256_context sha_ctx;
  mbedtls_sha256_init(&sha_ctx);
  mbedtls_sha256_starts_ret(&sha_ctx, 0);

  WiFiClient* stream = http->getStreamPtr();

  const int BUFFER_SIZE = 4096;
  int bytes_written = 0;
  int prev_progress = 0;

  unsigned long last_update = millis();
  unsigned long download_start = millis();
  unsigned long last_mqtt_loop = millis();  // 上次调用 mqtt_loop 的时间

  // LED 提示：下载中
  led1_set(LED_FAST_BLINK);

  unsigned long last_data_time = millis();  // 上次收到数据的时间
  const unsigned long NO_DATA_TIMEOUT = 120000;  // 120秒无数据超时（服务器可能暂停发送）

  // 流式读取
  while (http->connected() && (content_length > 0 || stream->available())) {

    if (g_cancel_requested) {
      Update.abort();
      led1_set(LED_OFF);
      finish_cancelled_ota("esp");
      return;
    }

    // 检查总超时
    if (millis() - download_start > 300000) {
      Serial.println("[MQTT-OTA-ESP] 下载总超时");
      Update.abort();
      mqtt_ota_report_error("esp", "DOWNLOAD_TIMEOUT", "下载超时（300s）");
      led1_set(LED_OFF);
      reset_ota_state();
      return;
    }

    // 检查无数据超时（服务器可能断开但连接状态未更新）
    if (millis() - last_data_time > NO_DATA_TIMEOUT) {
      Serial.printf("[MQTT-OTA-ESP] %lu 秒未收到数据，中断下载\n", NO_DATA_TIMEOUT / 1000);
      Update.abort();
      mqtt_ota_report_error("esp", "NO_DATA_TIMEOUT", "长时间未收到数据");
      led1_set(LED_OFF);
      reset_ota_state();
      return;
    }

    // 处理 UART（防止 ARM 心跳超时）
    uart_protocol_process();

    // 降低 mqtt_loop 调用频率：每 5 秒调用一次（避免阻塞 HTTP 流）
    if (millis() - last_mqtt_loop > 5000) {
      mqtt_loop();
      last_mqtt_loop = millis();
    }

    // 读取数据：每次读满 buffer（4096字节），减少 flash write 次数
    size_t available = stream->available();
    if (available > 0) {
      int to_read = min((int)available, (int)sizeof(g_ota_buffer));
      int bytes_read = stream->read(g_ota_buffer, to_read);

      if (bytes_read > 0) {
        last_data_time = millis();  // 更新数据接收时间
        // SHA-256 更新
        mbedtls_sha256_update_ret(&sha_ctx, g_ota_buffer, bytes_read);

        // 写入 Flash
        size_t written = Update.write(g_ota_buffer, bytes_read);
        if (written != bytes_read) {
          Serial.printf("[MQTT-OTA-ESP] 写入失败\n");
          Update.abort();
          mqtt_ota_report_error("esp", "WRITE_FAILED", "Flash 写入失败");
          led1_set(LED_OFF);
          reset_ota_state();
          return;
        }
        yield();  // flash 写入后让 TCP 栈处理积压数据

        bytes_written += bytes_read;
        g_ota_downloaded_bytes = bytes_written;

        // 已接收完所有数据，立即退出循环
        if (bytes_written >= content_length) {
          break;
        }

        // 每秒上报进度
        if (millis() - last_update > 1000) {
          int progress = (bytes_written * 100) / content_length;
          if (progress != prev_progress) {
            prev_progress = progress;

            int elapsed = (millis() - download_start) / 1000;
            int speed_kbps = (bytes_written / 1024) / (elapsed + 1);

            Serial.printf("[MQTT-OTA-ESP] 进度: %d%% (%d/%d), 速度: %d KB/s\n",
              progress, bytes_written, content_length, speed_kbps);

            mqtt_ota_report_status("esp", PROTO_OTA_STATE_DOWNLOADING, progress,
              PROTO_ERR_OK, String("下载中: " + String(progress) + "%").c_str());
          }
          last_update = millis();
        }
      } else if (bytes_read < 0) {
        Serial.printf("[MQTT-OTA-ESP] read() 返回错误: %d\n", bytes_read);
        break;
      }
    } else {
      // 每5秒打印一次等待状态
      static unsigned long last_wait_log = 0;
      if (millis() - last_wait_log > 5000) {
        last_wait_log = millis();
        unsigned long no_data_sec = (millis() - last_data_time) / 1000;
        Serial.printf("[MQTT-OTA-ESP] 等待数据... (已等待 %lu 秒, connected=%d, heap=%d)\n",
          no_data_sec, http->connected(), ESP.getFreeHeap());
      }
      yield();  // 让 WiFi/TCP 栈处理积压数据
      delay(50); // 给 TCP 栈足够时间填充 WiFiClient buffer
    }
  }

  Serial.printf("[MQTT-OTA-ESP] 下载完成, 共 %d 字节\n", bytes_written);

  if (g_cancel_requested) {
    Update.abort();
    led1_set(LED_OFF);
    finish_cancelled_ota("esp");
    return;
  }
  // 下载完成后，确保 MQTT 连接正常
  if (!mqtt_is_connected()) {
    Serial.println("[MQTT-OTA-ESP] MQTT 断连，尝试强制重连...");
    mqtt_connect_ex(true);  // 强制重连
    delay(100);             // 等待重连完成
  }

  mqtt_ota_report_status("esp", PROTO_OTA_STATE_VERIFYING, 100,
                         PROTO_ERR_OK, "校验中...");

  // LED 提示：校验中
  led1_set(LED_SOLID);

  // SHA-256 校验
  if (cmd->sha256[0] != '\0') {
    uint8_t hash[32];
    mbedtls_sha256_finish_ret(&sha_ctx, hash);
    mbedtls_sha256_free(&sha_ctx);

    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", hash[i]);

    if (strcasecmp(hex, cmd->sha256) != 0) {
      Serial.printf("[MQTT-OTA-ESP] SHA-256 校验失败: expected=%s, got=%s\n", cmd->sha256, hex);
      Update.abort();
      mqtt_ota_report_error("esp", PROTO_ERR_EXEC_FAILED, "SHA-256 校验失败");
      led1_set(LED_OFF);
      reset_ota_state();
      return;
    }
    Serial.println("[MQTT-OTA-ESP] SHA-256 校验通过");
  } else {
    mbedtls_sha256_free(&sha_ctx);
    Serial.println("[MQTT-OTA-ESP] 未提供 SHA-256，跳过校验");
  }

  // 结束 Update，触发校验和重启
  if (!Update.end(true)) {
    Serial.printf("[MQTT-OTA-ESP] 校验失败: %s\n", Update.errorString());
    mqtt_ota_report_error("esp", "VERIFY_FAILED", "固件校验失败");
    led1_set(LED_OFF);
    reset_ota_state();
    return;
  }

  Serial.println("[MQTT-OTA-ESP] 升级成功，设备即将重启");
  mqtt_ota_report_status("esp", PROTO_OTA_STATE_REBOOTING, 100,
                         PROTO_ERR_OK, "升级成功，重启中");
  // 保存结果到NVS，重启后APP可通过HTTP轮询读取
  ota_task_result_save(g_current_task_id.c_str(), "esp", "done",
                       FIRMWARE_VERSION, "ESP升级完成");
  g_persisted_ota_task = g_current_task_id;
  g_persisted_ota_target = "esp";
  g_persisted_ota_status = "done";
  Serial.printf("[MQTT-OTA] Result saved to NVS\n");
  led1_set(LED_OFF);

  // 重启前等待消息发送
  delay(1000);
  ESP.restart();
}

// ==================== ARM OTA 进度回调 ====================

static unsigned long s_arm_mqtt_last_report = 0;
static int s_arm_mqtt_last_progress = -1;

static void arm_ota_progress_callback(ArmOtaState state, int progress, const char* msg) {
  // 限制上报频率：进度变化 >= 5% 或 间隔 >= 3 秒才上报
  unsigned long now = millis();
  if (abs(progress - s_arm_mqtt_last_progress) >= 5 || now - s_arm_mqtt_last_report >= 3000) {
    s_arm_mqtt_last_progress = progress;
    s_arm_mqtt_last_report = now;
    mqtt_ota_report_status("arm", PROTO_OTA_STATE_DOWNLOADING, progress,
      PROTO_ERR_OK, String("ARM传输中: " + String(progress) + "%").c_str());
  }
}

// ==================== ARM OTA 下载处理 ====================

static void handle_arm_ota_download(HTTPClient* http, int content_length,
                                     const struct MqttOtaCommand* cmd) {

  Serial.println("[MQTT-OTA-ARM] 开始流式下载和转发");

  if (cmd->md5[0]) {
    uint8_t expected_md5[ARM_OTA_MD5_SIZE];
    for (int i = 0; i < ARM_OTA_MD5_SIZE; ++i) {
      char pair[3] = {cmd->md5[i * 2], cmd->md5[i * 2 + 1], 0};
      expected_md5[i] = (uint8_t)strtoul(pair, nullptr, 16);
    }
    if (!arm_ota_set_expected_md5(expected_md5)) {
      mqtt_ota_report_error("arm", "MD5_INIT_FAILED", "Unable to set ARM MD5");
      reset_ota_state();
      return;
    }
  }

  // 启动流式 OTA
  if (!arm_ota_start_streaming(content_length)) {
    Serial.println("[MQTT-OTA-ARM] arm_ota_start_streaming() 失败");
    mqtt_ota_report_error("arm", "ARM_OTA_START", "ARM OTA 初始化失败");
    reset_ota_state();
    return;
  }

  // 设置进度回调
  s_arm_mqtt_last_progress = -1;
  s_arm_mqtt_last_report = 0;
  arm_ota_set_callback(arm_ota_progress_callback);

  WiFiClient* stream = http->getStreamPtr();

  mbedtls_sha256_context arm_sha_ctx;
  mbedtls_sha256_init(&arm_sha_ctx);
  mbedtls_sha256_starts_ret(&arm_sha_ctx, 0);

  const int BUFFER_SIZE = 4096;
  uint8_t buffer[BUFFER_SIZE];
  int bytes_sent = 0;
  int prev_http_progress = 0;

  unsigned long last_update = millis();
  unsigned long download_start = millis();
  unsigned long last_mqtt_loop = millis();  // 上次调用 mqtt_loop 的时间

  // LED 提示
  led1_set(LED_FAST_BLINK);

  unsigned long last_data_time = millis();  // 上次收到数据的时间
  const unsigned long NO_DATA_TIMEOUT = 120000;  // 120秒无数据超时（服务器可能暂停发送）

  // 流式读取和转发
  while (http->connected() && (content_length > 0 || stream->available())) {

    if (g_cancel_requested) {
      arm_ota_cancel();
      mbedtls_sha256_free(&arm_sha_ctx);
      led1_set(LED_OFF);
      finish_cancelled_ota("arm");
      return;
    }

    // 检查总超时
    if (millis() - download_start > 600000) {
      Serial.println("[MQTT-OTA-ARM] 总超时");
      arm_ota_cancel();
      mqtt_ota_report_error("arm", "TIMEOUT", "传输超时");
      led1_set(LED_OFF);
      mbedtls_sha256_free(&arm_sha_ctx);
      reset_ota_state();
      return;
    }

    // 检查无数据超时
    if (millis() - last_data_time > NO_DATA_TIMEOUT) {
      Serial.printf("[MQTT-OTA-ARM] %lu 秒未收到数据，中断下载\n", NO_DATA_TIMEOUT / 1000);
      arm_ota_cancel();
      mqtt_ota_report_error("arm", "NO_DATA_TIMEOUT", "长时间未收到数据");
      led1_set(LED_OFF);
      mbedtls_sha256_free(&arm_sha_ctx);
      reset_ota_state();
      return;
    }

    // 处理 UART（必须频繁调用，接收 ARM 的 ACK）
    uart_protocol_process();

    // 驱动 ARM OTA 状态机（发送 DATA 包、等待 ACK 等）
    // 进度通过回调 arm_ota_progress_callback() 上报 MQTT
    arm_ota_process();

    // 降低 mqtt_loop 调用频率：每 5 秒调用一次（避免阻塞 HTTP 流）
    if (millis() - last_mqtt_loop > 5000) {
      mqtt_loop();
      last_mqtt_loop = millis();
    }

    // 读取数据：加大每次读取量到 1024 字节，快速填充 ARM OTA 缓冲区
    size_t available = stream->available();
    if (available > 0) {
      int to_read = min((int)available, 1024);  // 从 256 改为 1024
      int bytes_read = stream->read(buffer, to_read);

      if (bytes_read > 0) {
        last_data_time = millis();  // 更新数据接收时间
        mbedtls_sha256_update_ret(&arm_sha_ctx, buffer, bytes_read);

        if (!arm_ota_feed(buffer, bytes_read)) {
          Serial.println("[MQTT-OTA-ARM] arm_ota_feed() 失败");
          mqtt_ota_report_error("arm", "FEED_FAILED", "ARM 缓冲失败");
          led1_set(LED_OFF);
          mbedtls_sha256_free(&arm_sha_ctx);
          arm_ota_cancel();
          reset_ota_state();
          return;
        }

        bytes_sent += bytes_read;
        g_ota_downloaded_bytes = bytes_sent;

        // HTTP 下载进度（仅串口日志，MQTT 由 ARM 端回调上报）
        if (millis() - last_update > 2000) {
          int http_progress = (bytes_sent * 100) / content_length;
          if (http_progress != prev_http_progress) {
            prev_http_progress = http_progress;

            int elapsed = (millis() - download_start) / 1000;
            int speed_kbps = (bytes_sent / 1024) / (elapsed + 1);

            Serial.printf("[MQTT-OTA-ARM] HTTP下载: %d%% (%d/%d), ARM进度: %d%%, 速度: %d KB/s\n",
              http_progress, bytes_sent, content_length, arm_ota_get_progress(), speed_kbps);
          }
          last_update = millis();
        }
      } else if (bytes_read < 0) {
        // read() 返回负数表示错误
        Serial.printf("[MQTT-OTA-ARM] read() 返回错误: %d\n", bytes_read);
        break;
      }
    } else {
      delay(10);
    }
  }

  // 清除回调
  arm_ota_set_callback(NULL);

  Serial.printf("[MQTT-OTA-ARM] HTTP下载完成, 共 %d 字节, ARM进度: %d%%\n",
    bytes_sent, arm_ota_get_progress());

  if (g_cancel_requested) {
    arm_ota_cancel();
    mbedtls_sha256_free(&arm_sha_ctx);
    led1_set(LED_OFF);
    finish_cancelled_ota("arm");
    return;
  }

  if (!ota_security_version_advance(cmd->security_version)) {
    Update.rollBack();
    mqtt_ota_report_error("esp", "SECURITY_VERSION_SAVE_FAILED",
                          "Failed to persist anti-rollback version");
    led1_set(LED_OFF);
    reset_ota_state();
    return;
  }

  uint8_t arm_hash[32];
  char arm_hash_hex[65] = {0};
  mbedtls_sha256_finish_ret(&arm_sha_ctx, arm_hash);
  mbedtls_sha256_free(&arm_sha_ctx);
  for (int i = 0; i < 32; ++i) {
    snprintf(arm_hash_hex + i * 2, 3, "%02x", arm_hash[i]);
  }
  if (bytes_sent != content_length || strcasecmp(arm_hash_hex, cmd->sha256) != 0) {
    Serial.printf("[MQTT-OTA-ARM] SHA-256 mismatch: expected=%s got=%s\n",
                  cmd->sha256, arm_hash_hex);
    mqtt_ota_report_error("arm", "SHA256_MISMATCH", "ARM firmware SHA-256 mismatch");
    led1_set(LED_OFF);
    arm_ota_cancel();
    reset_ota_state();
    return;
  }

  // 下载完成后，确保 MQTT 连接正常
  if (!mqtt_is_connected()) {
    Serial.println("[MQTT-OTA-ARM] MQTT 断连，尝试强制重连...");
    mqtt_connect_ex(true);  // 强制重连
    delay(100);             // 等待重连完成
  }

  // 结束流式 OTA
  if (!arm_ota_end_streaming()) {
    Serial.println("[MQTT-OTA-ARM] arm_ota_end_streaming() 失败");
    mqtt_ota_report_error("arm", "END_FAILED", "ARM 结束失败");
    led1_set(LED_OFF);
    arm_ota_cancel();  // 清理 ARM OTA 状态
    reset_ota_state();
    return;
  }

  mqtt_ota_report_status("arm", PROTO_OTA_STATE_VERIFYING, 100,
                         PROTO_ERR_OK, "等待 ARM 校验");
  led1_set(LED_SOLID);

  Serial.println("[MQTT-OTA-ARM] 固件已转发，等待 ARM 校验结果");

  // 等待 ARM 校验完成（最多 30 秒）
  unsigned long verify_start = millis();
  const unsigned long VERIFY_TIMEOUT = 30000;

  while (millis() - verify_start < VERIFY_TIMEOUT) {
    if (g_cancel_requested) {
      arm_ota_cancel();
      led1_set(LED_OFF);
      finish_cancelled_ota("arm");
      return;
    }
    uart_protocol_process();
    arm_ota_process();

    ArmOtaState state = arm_ota_get_state();

    if (state == ARM_OTA_DONE) {
      Serial.println("[MQTT-OTA-ARM] ARM 校验成功，升级完成");
      if (!ota_security_version_advance(g_current_ota_cmd.security_version)) {
        mqtt_ota_report_error("arm", "SECURITY_VERSION_SAVE_FAILED",
                              "Failed to persist anti-rollback version");
        led1_set(LED_OFF);
        reset_ota_state();
        return;
      }
      mqtt_ota_report_status("arm", PROTO_OTA_STATE_SUCCEEDED, 100,
                             PROTO_ERR_OK, "升级成功");
      ota_task_result_save(g_current_task_id.c_str(), "arm", "done",
                           g_current_ota_cmd.version, "ARM升级完成");
      g_persisted_ota_task = g_current_task_id;
      g_persisted_ota_target = "arm";
      g_persisted_ota_status = "done";
      led1_set(LED_OFF);
      reset_ota_state();
      return;
    }

    if (state == ARM_OTA_ERROR || state == ARM_OTA_TIMEOUT) {
      Serial.printf("[MQTT-OTA-ARM] ARM 校验失败，状态=%d\n", state);
      mqtt_ota_report_error("arm", "VERIFY_FAILED", arm_ota_get_message());
      led1_set(LED_OFF);
      arm_ota_cancel();
      reset_ota_state();
      return;
    }

    delay(50);
  }

  // 超时
  Serial.println("[MQTT-OTA-ARM] ARM 校验超时");
  mqtt_ota_report_error("arm", "VERIFY_TIMEOUT", "ARM 校验超时");
  led1_set(LED_OFF);
  arm_ota_cancel();
  reset_ota_state();
}

// ==================== 状态上报 ====================

void mqtt_ota_report_status(const char* target, const char* state, int progress,
                            const char* code, const char* detail) {

  StaticJsonDocument<256> doc;
  doc["task_id"] = g_current_task_id.length()
      ? g_current_task_id.c_str() : g_current_ota_cmd.task_id;
  doc["target"] = target;
  doc["version"] = g_current_ota_cmd.version[0] ? g_current_ota_cmd.version : FIRMWARE_VERSION;
  doc["state"] = state;
  doc["progress"] = progress;
  doc["code"] = code ? code : PROTO_ERR_OK;
  doc["message"] = detail ? detail : "";

  String payload;
  serializeJson(doc, payload);

  // 确保 MQTT 连接正常
  if (!mqtt_is_connected()) {
    Serial.println("[MQTT-OTA] MQTT 断连，尝试强制重连后上报...");
    mqtt_connect_ex(true);  // 强制重连
    delay(100);             // 等待重连完成
  }

  mqtt_publish_envelope(PROTO_TOPIC_OTA_STATUS, payload.c_str(), payload.length(), false);

  Serial.printf("[MQTT-OTA] 上报状态: %s\n", payload.c_str());
}

void mqtt_ota_report_error(const char* target, const char* error_code,
                           const char* error_message) {

  StaticJsonDocument<256> doc;
  doc["task_id"] = g_current_task_id.length()
      ? g_current_task_id.c_str() : g_current_ota_cmd.task_id;
  doc["target"] = target;
  doc["version"] = g_current_ota_cmd.version[0] ? g_current_ota_cmd.version : FIRMWARE_VERSION;
  doc["state"] = PROTO_OTA_STATE_FAILED;
  doc["progress"] = 0;
  doc["code"] = error_code ? error_code : PROTO_ERR_EXEC_FAILED;
  doc["message"] = error_message ? error_message : "";

  String payload;
  serializeJson(doc, payload);

  // 确保 MQTT 连接正常
  if (!mqtt_is_connected()) {
    Serial.println("[MQTT-OTA] MQTT 断连，尝试强制重连后上报错误...");
    mqtt_connect_ex(true);  // 强制重连
    delay(100);             // 等待重连完成
  }

  mqtt_publish_envelope(PROTO_TOPIC_OTA_STATUS, payload.c_str(), payload.length(), false);

  if (g_current_task_id.length()) {
    ota_task_result_save(g_current_task_id.c_str(), target, "failed",
                         g_current_ota_cmd.version, error_message);
    g_persisted_ota_task = g_current_task_id;
    g_persisted_ota_target = target ? target : "";
    g_persisted_ota_status = "failed";
  }

  Serial.printf("[MQTT-OTA] 上报错误: %s\n", payload.c_str());
}

static void send_ota_ack(const char* task_id, bool ack, const char* message) {
  StaticJsonDocument<256> doc;
  doc["task_id"] = task_id ? task_id : "";
  doc["cmd"] = "ota";
  doc["stage"] = ack ? PROTO_CMD_STAGE_ACK : PROTO_CMD_STAGE_DONE;
  doc["success"] = ack;
  doc["code"] = ack ? PROTO_ERR_OK : PROTO_ERR_EXEC_FAILED;
  doc["message"] = message ? message : "";
  doc["result"].to<JsonArray>();

  String payload;
  serializeJson(doc, payload);

  mqtt_publish_envelope(PROTO_TOPIC_CMD_RESP, payload.c_str(), payload.length(), false);

  Serial.printf("[MQTT-OTA] 发送 ACK: %s\n", payload.c_str());
}

static void send_ota_reject(const char* task_id, const char* code, const char* message) {
  StaticJsonDocument<256> doc;
  doc["task_id"] = task_id ? task_id : "";
  doc["cmd"] = "ota";
  doc["stage"] = PROTO_CMD_STAGE_DONE;
  doc["success"] = false;
  doc["code"] = code ? code : PROTO_ERR_EXEC_FAILED;
  doc["message"] = message ? message : "";
  doc["result"].to<JsonArray>();
  String payload;
  serializeJson(doc, payload);
  mqtt_publish_envelope(PROTO_TOPIC_CMD_RESP, payload.c_str(), payload.length(), false);
}

static void send_persisted_ota_result(const char* task_id) {
  StaticJsonDocument<256> doc;
  doc["task_id"] = task_id ? task_id : "";
  doc["cmd"] = "ota";
  doc["stage"] = PROTO_CMD_STAGE_DONE;
  doc["success"] = g_persisted_ota_status == "done";
  doc["code"] = doc["success"].as<bool>() ? PROTO_ERR_OK : PROTO_ERR_EXEC_FAILED;
  doc["message"] = "Persisted OTA result replay";
  JsonObject result = doc["result"].to<JsonObject>();
  result["target"] = g_persisted_ota_target;
  result["state"] = g_persisted_ota_status;
  String payload;
  serializeJson(doc, payload);
  mqtt_publish_envelope(PROTO_TOPIC_CMD_RESP, payload.c_str(), payload.length(), false);
}

static void send_ota_task_result(const char* task_id, const char* target,
                                 const char* state, bool success, const char* message) {
  StaticJsonDocument<320> doc;
  doc["task_id"] = task_id ? task_id : "";
  doc["cmd"] = "ota";
  doc["stage"] = PROTO_CMD_STAGE_DONE;
  doc["success"] = success;
  doc["code"] = success ? PROTO_ERR_OK : PROTO_ERR_EXEC_FAILED;
  doc["message"] = message ? message : "";
  JsonObject result = doc["result"].to<JsonObject>();
  result["target"] = target ? target : "";
  result["state"] = state ? state : "";
  String payload;
  serializeJson(doc, payload);
  mqtt_publish_envelope(PROTO_TOPIC_CMD_RESP, payload.c_str(), payload.length(), false);
}

// ==================== 状态查询 ====================

void mqtt_ota_process() {
  // 处理 UART 以保持 ARM 心跳
  uart_protocol_process();

  // 如果有流式 OTA 在进行，需要定期处理
  arm_ota_process();

  // 如果有待执行的 OTA 下载，在主循环中启动（不在 MQTT 回调中阻塞）
  if (g_download_pending) {
    g_download_pending = false;
    if (g_cancel_requested) {
      finish_cancelled_ota(g_current_ota_target.c_str());
      return;
    }
    Serial.println("[MQTT-OTA] 主循环中启动固件下载");
    start_firmware_download(&g_current_ota_cmd);
  }
}

bool mqtt_ota_is_busy() {
  return g_ota_cmd_received && (millis() - g_ota_cmd_time < 600000);
}

const char* mqtt_ota_get_task_id() {
  return g_current_task_id.c_str();
}
