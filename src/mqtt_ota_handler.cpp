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
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <string.h>

// ==================== OTA 大小限制 ====================
#define OTA_ESP_MAX_SIZE  (2 * 1024 * 1024)      // ESP: 2 MB
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

// OTA 下载缓冲区（静态分配，避免栈溢出）
static uint8_t g_ota_buffer[4096];

// ==================== 前向声明 ====================
static bool parse_ota_command(const uint8_t* payload, unsigned int len, struct MqttOtaCommand* cmd);
static void handle_ota_start_command(const struct MqttOtaCommand* cmd);
static void handle_ota_stop_command(const struct MqttOtaCommand* cmd);
static void handle_ota_query_command(const struct MqttOtaCommand* cmd);
static void send_ota_ack(const char* task_id, bool ack, const char* message);
static void start_firmware_download(const struct MqttOtaCommand* cmd);
static void handle_esp_ota_download(HTTPClient* http, int content_length, const struct MqttOtaCommand* cmd);
static void handle_arm_ota_download(HTTPClient* http, int content_length, const struct MqttOtaCommand* cmd);

// ==================== 初始化 ====================

void mqtt_ota_init() {
  Serial.println("[MQTT-OTA] 模块已初始化");

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
  // 确保 payload 以 null 结尾
  static char json_buf[512];
  memset(json_buf, 0, sizeof(json_buf));
  strncpy(json_buf, (const char*)payload, min(len, sizeof(json_buf) - 1));

  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, json_buf);

  if (error) {
    Serial.printf("[MQTT-OTA] JSON 解析失败: %s\n", error.c_str());
    return false;
  }

  // 提取字段
  strncpy(cmd->command, doc["command"] | "", sizeof(cmd->command) - 1);
  strncpy(cmd->target, doc["target"] | "", sizeof(cmd->target) - 1);
  strncpy(cmd->firmware_id, doc["firmware_id"] | "", sizeof(cmd->firmware_id) - 1);
  strncpy(cmd->url, doc["url"] | "", sizeof(cmd->url) - 1);
  strncpy(cmd->version, doc["version"] | "", sizeof(cmd->version) - 1);
  strncpy(cmd->md5, doc["md5"] | "", sizeof(cmd->md5) - 1);
  strncpy(cmd->task_id, doc["task_id"] | "", sizeof(cmd->task_id) - 1);

  // 去除 URL 两端的空格（服务器可能发送带空格的 URL）
  trim_str(cmd->url);

  cmd->file_size = doc["file_size"] | 0;
  cmd->timeout = doc["timeout"] | 300;
  cmd->timestamp = doc["timestamp"] | (uint32_t)time(nullptr);

  return true;
}

// ==================== 命令处理 ====================

bool mqtt_ota_handle_command(const char* topic, const uint8_t* payload, unsigned int len) {

  Serial.printf("[MQTT-OTA] 收到命令 (主题: %s, 长度: %d)\n", topic, len);
  Serial.printf("[MQTT-OTA] Payload: %.*s\n", (int)len, payload);

  // 解析 JSON
  struct MqttOtaCommand cmd = {0};
  if (!parse_ota_command(payload, len, &cmd)) {
    mqtt_ota_report_error("unknown", "JSON_ERROR", "命令格式错误");
    return false;
  }

  // 验证必要字段
  if (strlen(cmd.command) == 0) {
    mqtt_ota_report_error("unknown", "MISSING_FIELD", "缺少 command 字段");
    return false;
  }

  Serial.printf("[MQTT-OTA] 命令: %s, 目标: %s, URL: %s\n",
    cmd.command, cmd.target, cmd.url);

  // 保存当前命令
  memcpy(&g_current_ota_cmd, &cmd, sizeof(cmd));

  // 根据命令类型处理
  if (strcmp(cmd.command, "start") == 0) {
    handle_ota_start_command(&cmd);
  } else if (strcmp(cmd.command, "stop") == 0) {
    handle_ota_stop_command(&cmd);
  } else if (strcmp(cmd.command, "query") == 0) {
    handle_ota_query_command(&cmd);
  } else {
    Serial.printf("[MQTT-OTA] 未知命令: %s\n", cmd.command);
    mqtt_ota_report_error(cmd.target, "UNKNOWN_CMD", "命令不支持");
  }
  return true;  // OTA 命令已处理（即使处理失败，也不应转发给 ARM）
}

// ==================== 命令处理函数 ====================

static void handle_ota_start_command(const struct MqttOtaCommand* cmd) {

  Serial.printf("[MQTT-OTA] 处理 START 命令\n");

  // 验证参数
  if (strlen(cmd->target) == 0 || strlen(cmd->url) == 0) {
    mqtt_ota_report_error(cmd->target, "INVALID_PARAMS", "缺少 target 或 url");
    return;
  }

  if (strcmp(cmd->target, "esp") != 0 && strcmp(cmd->target, "arm") != 0) {
    mqtt_ota_report_error(cmd->target, "INVALID_TARGET", "target 只能是 esp 或 arm");
    return;
  }

  // 检查是否已有 OTA 在进行
  if (mqtt_ota_is_busy()) {
    mqtt_ota_report_error(cmd->target, "OTA_BUSY", "已有 OTA 操作进行中");
    return;
  }

  // 保存任务信息
  g_current_task_id = cmd->task_id;
  g_current_ota_target = cmd->target;
  g_ota_total_bytes = cmd->file_size;
  g_ota_downloaded_bytes = 0;
  g_ota_start_time = millis();
  g_ota_cmd_received = true;
  g_ota_cmd_time = millis();

  // 发送应答
  send_ota_ack(cmd->task_id, true, "OTA 命令已接收");

  // 立即上报状态：准备下载
  mqtt_ota_report_status(cmd->target, "preparing", 0, "准备下载固件");

  // 延迟一下再开始下载，给 MQTT 消息发送的时间
  delay(100);

  // 启动下载
  start_firmware_download(cmd);
}

static void handle_ota_stop_command(const struct MqttOtaCommand* cmd) {
  Serial.printf("[MQTT-OTA] 处理 STOP 命令\n");

  // 暂时未实现完全的取消功能，留给后续扩展
  Serial.println("[MQTT-OTA] STOP 命令暂不支持");
}

static void handle_ota_query_command(const struct MqttOtaCommand* cmd) {
  Serial.printf("[MQTT-OTA] 处理 QUERY 命令\n");

  // 上报当前 OTA 状态
  if (mqtt_ota_is_busy()) {
    mqtt_ota_report_status(g_current_ota_target.c_str(), "downloading", 50, "查询中");
  } else {
    mqtt_ota_report_status("esp", "idle", 0, "空闲");
  }
}

// ==================== 重置 OTA 状态 ====================

static void reset_ota_state() {
  g_ota_cmd_received = false;
  g_current_task_id = "";
  g_current_ota_target = "";
  g_ota_total_bytes = 0;
  g_ota_downloaded_bytes = 0;
  Serial.println("[MQTT-OTA] 状态已重置");
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

  if (content_length <= 0) {
    Serial.println("[MQTT-OTA] 无效的内容长度");
    mqtt_ota_report_error(cmd->target, "INVALID_SIZE", "无效的内容长度");
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

  WiFiClient* stream = http->getStreamPtr();

  const int BUFFER_SIZE = 4096;
  int bytes_written = 0;
  int prev_progress = 0;

  unsigned long last_update = millis();
  unsigned long download_start = millis();

  // LED 提示：下载中
  led1_set(LED_FAST_BLINK);

  // 流式读取
  while (http->connected() && (content_length > 0 || stream->available())) {

    // 检查超时
    if (millis() - download_start > 300000) {
      Serial.println("[MQTT-OTA-ESP] 下载超时");
      Update.abort();
      mqtt_ota_report_error("esp", "DOWNLOAD_TIMEOUT", "下载超时（300s）");
      led1_set(LED_OFF);
      reset_ota_state();
      return;
    }

    // 处理 UART（防止 ARM 心跳超时）
    uart_protocol_process();

    // 读取数据
    size_t available = stream->available();
    if (available > 0) {
      int to_read = min((int)available, BUFFER_SIZE);
      int bytes_read = stream->readBytes(g_ota_buffer, to_read);

      if (bytes_read > 0) {
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

        bytes_written += bytes_read;
        g_ota_downloaded_bytes = bytes_written;

        // 每秒上报进度
        if (millis() - last_update > 1000) {
          int progress = (bytes_written * 100) / content_length;
          if (progress != prev_progress) {
            prev_progress = progress;

            int elapsed = (millis() - download_start) / 1000;
            int speed_kbps = (bytes_written / 1024) / (elapsed + 1);

            Serial.printf("[MQTT-OTA-ESP] 进度: %d%% (%d/%d), 速度: %d KB/s\n",
              progress, bytes_written, content_length, speed_kbps);

            mqtt_ota_report_status("esp", "downloading", progress,
              String("下载中: " + String(progress) + "%").c_str());
          }
          last_update = millis();
        }
      }
    } else {
      delay(10);
    }
  }

  Serial.printf("[MQTT-OTA-ESP] 下载完成, 共 %d 字节\n", bytes_written);
  mqtt_ota_report_status("esp", "verifying", 100, "校验中...");

  // LED 提示：校验中
  led1_set(LED_SOLID);

  // 结束 Update，触发校验和重启
  if (!Update.end(true)) {
    Serial.printf("[MQTT-OTA-ESP] 校验失败: %s\n", Update.errorString());
    mqtt_ota_report_error("esp", "VERIFY_FAILED", "固件校验失败");
    led1_set(LED_OFF);
    reset_ota_state();
    return;
  }

  Serial.println("[MQTT-OTA-ESP] 升级成功，设备即将重启");
  mqtt_ota_report_status("esp", "done", 100, "升级成功，重启中");
  // 保存结果到NVS，重启后APP可通过HTTP轮询读取
  ota_result_save("done", FIRMWARE_VERSION, "ESP升级完成");
  Serial.printf("[MQTT-OTA] Result saved to NVS\n");
  led1_set(LED_OFF);

  // 重启前等待消息发送
  delay(1000);
  ESP.restart();
}

// ==================== ARM OTA 下载处理 ====================

static void handle_arm_ota_download(HTTPClient* http, int content_length,
                                     const struct MqttOtaCommand* cmd) {

  Serial.println("[MQTT-OTA-ARM] 开始流式下载和转发");

  // 启动流式 OTA
  if (!arm_ota_start_streaming(content_length)) {
    Serial.println("[MQTT-OTA-ARM] arm_ota_start_streaming() 失败");
    mqtt_ota_report_error("arm", "ARM_OTA_START", "ARM OTA 初始化失败");
    reset_ota_state();
    return;
  }

  WiFiClient* stream = http->getStreamPtr();

  const int BUFFER_SIZE = 1024;
  uint8_t buffer[BUFFER_SIZE];
  int bytes_sent = 0;
  int prev_progress = 0;

  unsigned long last_update = millis();
  unsigned long download_start = millis();

  // LED 提示
  led1_set(LED_FAST_BLINK);

  // 流式读取和转发
  while (http->connected() && (content_length > 0 || stream->available())) {

    if (millis() - download_start > 600000) {
      Serial.println("[MQTT-OTA-ARM] 总超时");
      arm_ota_cancel();
      mqtt_ota_report_error("arm", "TIMEOUT", "传输超时");
      led1_set(LED_OFF);
      reset_ota_state();
      return;
    }

    // 处理 UART
    uart_protocol_process();

    size_t available = stream->available();
    if (available > 0) {
      int to_read = min((int)available, BUFFER_SIZE);
      int bytes_read = stream->readBytes(buffer, to_read);

      if (bytes_read > 0) {
        if (!arm_ota_feed(buffer, bytes_read)) {
          Serial.println("[MQTT-OTA-ARM] arm_ota_feed() 失败");
          mqtt_ota_report_error("arm", "FEED_FAILED", "ARM 缓冲失败");
          led1_set(LED_OFF);
          reset_ota_state();
          return;
        }

        bytes_sent += bytes_read;
        g_ota_downloaded_bytes = bytes_sent;

        if (millis() - last_update > 1000) {
          int progress = (bytes_sent * 100) / content_length;
          if (progress != prev_progress) {
            prev_progress = progress;

            int elapsed = (millis() - download_start) / 1000;
            int speed_kbps = (bytes_sent / 1024) / (elapsed + 1);

            Serial.printf("[MQTT-OTA-ARM] 进度: %d%% (%d/%d), 速度: %d KB/s\n",
              progress, bytes_sent, content_length, speed_kbps);

            mqtt_ota_report_status("arm", "downloading", progress,
              String("下载中: " + String(progress) + "%").c_str());
          }
          last_update = millis();
        }
      }
    } else {
      delay(10);
    }
  }

  Serial.printf("[MQTT-OTA-ARM] 下载完成, 共 %d 字节\n", bytes_sent);

  // 结束流式 OTA
  if (!arm_ota_end_streaming()) {
    Serial.println("[MQTT-OTA-ARM] arm_ota_end_streaming() 失败");
    mqtt_ota_report_error("arm", "END_FAILED", "ARM 结束失败");
    led1_set(LED_OFF);
    reset_ota_state();
    return;
  }

  mqtt_ota_report_status("arm", "verifying", 100, "等待 ARM 校验");
  led1_set(LED_SOLID);

  Serial.println("[MQTT-OTA-ARM] 固件已转发，等待 ARM 校验");
}

// ==================== 状态上报 ====================

void mqtt_ota_report_status(const char* target, const char* state, int progress,
                            const char* detail) {

  StaticJsonDocument<256> doc;
  doc["device_id"] = mqtt_get_client_id();
  doc["current_version"] = FIRMWARE_VERSION;
  doc["state"] = state;
  doc["progress"] = progress;
  doc["status_message"] = detail;
  doc["error_message"] = "";

  String payload;
  serializeJson(doc, payload);

  mqtt_publish("ota/status", payload.c_str(), payload.length(), 0, false);

  Serial.printf("[MQTT-OTA] 上报状态: %s\n", payload.c_str());
}

void mqtt_ota_report_error(const char* target, const char* error_code,
                           const char* error_message) {

  StaticJsonDocument<256> doc;
  doc["device_id"] = mqtt_get_client_id();
  doc["current_version"] = FIRMWARE_VERSION;
  doc["state"] = "failed";
  doc["progress"] = 0;
  doc["status_message"] = "";
  doc["error_message"] = error_message;

  String payload;
  serializeJson(doc, payload);

  mqtt_publish("ota/status", payload.c_str(), payload.length(), 1, false);

  Serial.printf("[MQTT-OTA] 上报错误: %s\n", payload.c_str());
}

static void send_ota_ack(const char* task_id, bool ack, const char* message) {
  StaticJsonDocument<256> doc;
  doc["ack"] = ack;
  doc["task_id"] = task_id;
  doc["message"] = message;
  doc["timestamp"] = (uint32_t)time(nullptr);

  String payload;
  serializeJson(doc, payload);

  mqtt_publish("ota/cmd_ack", payload.c_str(), payload.length(), 1, false);

  Serial.printf("[MQTT-OTA] 发送 ACK: %s\n", payload.c_str());
}

// ==================== 状态查询 ====================

void mqtt_ota_process() {
  // 处理 UART 以保持 ARM 心跳
  uart_protocol_process();

  // 如果有流式 OTA 在进行，需要定期处理
  arm_ota_process();
}

bool mqtt_ota_is_busy() {
  return g_ota_cmd_received && (millis() - g_ota_cmd_time < 600000);
}

const char* mqtt_ota_get_task_id() {
  return g_current_task_id.c_str();
}
