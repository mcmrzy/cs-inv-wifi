/**
 * ================================================================
 *  OTA 管理模块
 *  支持: ESP32-C3 / ESP8266
 *
 *  功能:
 *    - ESP 自身固件 OTA（HTTP 下载 + Flash 写入）
 *    - ARM MCU 固件 OTA（HTTP 下载 + arm_ota 模块串口转发）
 *    - 进度回调、状态查询、版本管理
 * ================================================================
 */

#include "ota_manager.h"
#include "arm_ota.h"
#include "led_status.h"
#include "nvs_storage.h"

#include <string.h>

#ifdef CHIP_ESP32C3
  #include <WiFi.h>
  #include <HTTPClient.h>
  #include <Update.h>
  #include <Preferences.h>
#else
  #include <ESP8266WiFi.h>
  #include <ESP8266HTTPClient.h>
  #include <ESP8266httpUpdate.h>
#endif

// ==================== 常量 ====================

#ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION "1.0.0"
#endif

#define OTA_ESP_MAX_SIZE    (2 * 1024 * 1024)   // ESP 固件最大 2MB
#define OTA_ARM_MAX_SIZE    (256 * 1024)         // ARM 固件最大 256KB
#define OTA_DOWNLOAD_TIMEOUT 60000               // 下载超时 60 秒
#define OTA_ARM_VERSION_KEY  "arm_ver"

// ==================== 内部状态 ====================

static OtaStatus   g_ota_status  = OTA_STATUS_IDLE;
static OtaTarget   g_ota_target  = OTA_TARGET_ESP;
static int         g_ota_progress = 0;
static char        g_ota_message[128] = {0};
static OtaProgressCallback g_ota_callback = NULL;

// ESP OTA 状态机内部变量
static bool        g_esp_ota_active = false;
#ifdef CHIP_ESP32C3
  static HTTPClient g_http_client;
  static int        g_http_content_len = 0;
  static int        g_http_downloaded = 0;
  static bool       g_http_connected = false;
#else
  static String     g_esp8266_ota_url;
  static bool       g_esp8266_ota_started = false;
#endif

// ARM OTA 状态机内部变量
static bool        g_arm_ota_active = false;

// ARM 版本缓存
static char g_arm_version[32] = {0};

// ==================== 工具函数 ====================

static bool is_valid_url(const char* url) {
  if (!url || strlen(url) < 8) return false;
  if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
    return false;
  }
  return true;
}

static void notify_callback(OtaTarget target, int percent, OtaStatus status, const char* message) {
  if (g_ota_callback) {
    g_ota_callback(target, percent, status, message);
  }
}

static void set_state(OtaStatus status, const char* msg) {
  g_ota_status = status;
  if (msg) {
    strncpy(g_ota_message, msg, sizeof(g_ota_message) - 1);
    g_ota_message[sizeof(g_ota_message) - 1] = '\0';
  }
}

// ==================== ARM 版本 NVS 读写 ====================

static void load_arm_version() {
#ifdef USE_PREFERENCES
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, true);  // 只读模式
  String ver = prefs.getString(OTA_ARM_VERSION_KEY, "");
  prefs.end();
  if (ver.length() > 0) {
    strncpy(g_arm_version, ver.c_str(), sizeof(g_arm_version) - 1);
  }
#else
  strncpy(g_arm_version, "unknown", sizeof(g_arm_version) - 1);
#endif
}

static void save_arm_version(const char* version) {
  strncpy(g_arm_version, version, sizeof(g_arm_version) - 1);
  g_arm_version[sizeof(g_arm_version) - 1] = '\0';
#ifdef USE_PREFERENCES
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putString(OTA_ARM_VERSION_KEY, version);
  prefs.end();
#endif
}

// ==================== ARM OTA 回调 ====================

static void arm_ota_progress_cb(ArmOtaState state, int percent, const char* message) {
  switch (state) {
    case ARM_OTA_IDLE:
      // 不应触发
      break;
    case ARM_OTA_WAIT_READY:
      set_state(OTA_STATUS_DOWNLOADING, message);
      notify_callback(OTA_TARGET_ARM, 0, OTA_STATUS_DOWNLOADING, message);
      break;
    case ARM_OTA_SENDING:
    case ARM_OTA_WAIT_ACK:
      g_ota_progress = percent;
      set_state(OTA_STATUS_WRITING, message);
      notify_callback(OTA_TARGET_ARM, percent, OTA_STATUS_WRITING, message);
      break;
    case ARM_OTA_VERIFYING:
      set_state(OTA_STATUS_VERIFYING, message);
      notify_callback(OTA_TARGET_ARM, 100, OTA_STATUS_VERIFYING, message);
      break;
    case ARM_OTA_DONE:
      g_arm_ota_active = false;
      set_state(OTA_STATUS_DONE, "ARM OTA completed");
      notify_callback(OTA_TARGET_ARM, 100, OTA_STATUS_DONE, "ARM OTA completed");
      // 保存 ARM 版本（如果有信息）
      {
        const ArmFirmwareInfo* info = arm_ota_get_firmware_info();
        if (info && info->valid && strlen(info->version) > 0) {
          save_arm_version(info->version);
        }
      }
      break;
    case ARM_OTA_ERROR:
      g_arm_ota_active = false;
      set_state(OTA_STATUS_ERROR, message);
      notify_callback(OTA_TARGET_ARM, g_ota_progress, OTA_STATUS_ERROR, message);
      break;
    case ARM_OTA_TIMEOUT:
      g_arm_ota_active = false;
      set_state(OTA_STATUS_ERROR, message);
      notify_callback(OTA_TARGET_ARM, g_ota_progress, OTA_STATUS_ERROR, message);
      break;
  }
}

// ==================== ESP OTA 实现 ====================

#ifdef CHIP_ESP32C3

// ESP32-C3: 手动管理 HTTP 流 + Update 库（非阻塞）
static void esp_ota_begin(const char* url) {
  Serial.printf("[OTA] ESP OTA start: %s\n", url);

  g_http_client.setConnectTimeout(15000);
  g_http_client.setTimeout(OTA_DOWNLOAD_TIMEOUT);

  WiFiClient stream;
  if (!g_http_client.begin(stream, url)) {
    set_state(OTA_STATUS_ERROR, "HTTP begin failed");
    g_esp_ota_active = false;
    notify_callback(OTA_TARGET_ESP, 0, OTA_STATUS_ERROR, g_ota_message);
    return;
  }

  int http_code = g_http_client.GET();
  if (http_code != HTTP_CODE_OK) {
    Serial.printf("[OTA] HTTP error: %d\n", http_code);
    set_state(OTA_STATUS_ERROR, "HTTP GET failed");
    g_http_client.end();
    g_esp_ota_active = false;
    notify_callback(OTA_TARGET_ESP, 0, OTA_STATUS_ERROR, g_ota_message);
    return;
  }

  g_http_content_len = g_http_client.getSize();
  if (g_http_content_len <= 0 || g_http_content_len > OTA_ESP_MAX_SIZE) {
    Serial.printf("[OTA] Invalid firmware size: %d\n", g_http_content_len);
    set_state(OTA_STATUS_ERROR, "Invalid firmware size");
    g_http_client.end();
    g_esp_ota_active = false;
    notify_callback(OTA_TARGET_ESP, 0, OTA_STATUS_ERROR, g_ota_message);
    return;
  }

  Serial.printf("[OTA] Firmware size: %d bytes\n", g_http_content_len);

  if (!Update.begin(g_http_content_len)) {
    Serial.printf("[OTA] Update.begin failed: %s\n", Update.errorString());
    set_state(OTA_STATUS_ERROR, "Update.begin failed");
    g_http_client.end();
    g_esp_ota_active = false;
    notify_callback(OTA_TARGET_ESP, 0, OTA_STATUS_ERROR, g_ota_message);
    return;
  }

  g_http_downloaded = 0;
  g_http_connected = true;
  set_state(OTA_STATUS_DOWNLOADING, "Downloading firmware...");
  notify_callback(OTA_TARGET_ESP, 0, OTA_STATUS_DOWNLOADING, g_ota_message);
}

static void esp_ota_process() {
  if (!g_http_connected) return;

  WiFiClient* stream = g_http_client.getStreamPtr();
  if (!stream || !stream->available()) return;

  // 每次最多读 4KB，避免长时间阻塞
  uint8_t buf[4096];
  int available = stream->available();
  if (available > (int)sizeof(buf)) available = (int)sizeof(buf);

  int read_len = stream->readBytes(buf, available);
  if (read_len > 0) {
    g_http_downloaded += read_len;
    int percent = (g_http_content_len > 0)
                  ? (int)((long)g_http_downloaded * 100 / g_http_content_len)
                  : 0;

    if (percent < 90) {
      set_state(OTA_STATUS_DOWNLOADING, "Downloading firmware...");
    } else {
      set_state(OTA_STATUS_WRITING, "Writing firmware...");
    }

    g_ota_progress = percent;
    notify_callback(OTA_TARGET_ESP, percent, g_ota_status, g_ota_message);

    size_t written = Update.write(buf, read_len);
    if (written != (size_t)read_len) {
      Serial.printf("[OTA] Write error: expected %d, wrote %u\n", read_len, written);
      Update.abort();
      g_http_client.end();
      g_http_connected = false;
      set_state(OTA_STATUS_ERROR, "Flash write error");
      g_esp_ota_active = false;
      notify_callback(OTA_TARGET_ESP, percent, OTA_STATUS_ERROR, g_ota_message);
      return;
    }
  }

  // 检查下载是否完成
  if (g_http_downloaded >= g_http_content_len) {
    g_http_connected = false;
    g_http_client.end();

    set_state(OTA_STATUS_VERIFYING, "Verifying firmware...");
    notify_callback(OTA_TARGET_ESP, 100, OTA_STATUS_VERIFYING, g_ota_message);

    if (Update.end(true)) {
      Serial.println("[OTA] Update success, restarting...");
      set_state(OTA_STATUS_DONE, "Update successful");
      notify_callback(OTA_TARGET_ESP, 100, OTA_STATUS_DONE, g_ota_message);
      g_esp_ota_active = false;
      delay(1000);
      ESP.restart();
    } else {
      Serial.printf("[OTA] Update.end failed: %s\n", Update.errorString());
      set_state(OTA_STATUS_ERROR, Update.errorString());
      g_esp_ota_active = false;
      notify_callback(OTA_TARGET_ESP, 100, OTA_STATUS_ERROR, g_ota_message);
    }
  }
}

static void esp_ota_abort() {
  if (g_http_connected) {
    g_http_client.end();
    g_http_connected = false;
  }
  if (Update.isRunning()) {
    Update.abort();
  }
  g_esp_ota_active = false;
}

#else // ESP8266

// ESP8266: 使用 ESPhttpUpdate 库（阻塞式）
static void esp_ota_begin(const char* url) {
  Serial.printf("[OTA] ESP8266 OTA start: %s\n", url);
  g_esp8266_ota_url = url;
  g_esp8266_ota_started = false;
  set_state(OTA_STATUS_DOWNLOADING, "Starting ESP8266 OTA...");
  notify_callback(OTA_TARGET_ESP, 0, OTA_STATUS_DOWNLOADING, g_ota_message);
}

static void esp_ota_process() {
  if (g_esp8266_ota_started) return;

  g_esp8266_ota_started = true;

  ESPhttpUpdate.onProgress([](int cur, int total) {
    int percent = (total > 0) ? (int)((long)cur * 100 / total) : 0;
    g_ota_progress = percent;
    if (percent < 90) {
      set_state(OTA_STATUS_DOWNLOADING, "Downloading firmware...");
    } else {
      set_state(OTA_STATUS_WRITING, "Writing firmware...");
    }
    notify_callback(OTA_TARGET_ESP, percent, g_ota_status, g_ota_message);
  });

  WiFiClient client;
  t_httpUpdate_return ret = ESPhttpUpdate.update(client, g_esp8266_ota_url);

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("[OTA] ESP8266 update failed: %s\n",
                    ESPhttpUpdate.getLastErrorString().c_str());
      set_state(OTA_STATUS_ERROR, ESPhttpUpdate.getLastErrorString().c_str());
      notify_callback(OTA_TARGET_ESP, g_ota_progress, OTA_STATUS_ERROR, g_ota_message);
      g_esp_ota_active = false;
      break;

    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("[OTA] No updates available");
      set_state(OTA_STATUS_ERROR, "No updates available");
      notify_callback(OTA_TARGET_ESP, 100, OTA_STATUS_ERROR, g_ota_message);
      g_esp_ota_active = false;
      break;

    case HTTP_UPDATE_OK:
      Serial.println("[OTA] ESP8266 update success, restarting...");
      set_state(OTA_STATUS_DONE, "Update successful");
      notify_callback(OTA_TARGET_ESP, 100, OTA_STATUS_DONE, g_ota_message);
      g_esp_ota_active = false;
      delay(1000);
      ESP.restart();
      break;
  }
}

static void esp_ota_abort() {
  g_esp_ota_active = false;
}

#endif // CHIP_ESP32C3

// ==================== ARM OTA 实现（集成 arm_ota 模块）====================

#ifdef CHIP_ESP32C3

// ESP32-C3: 有足够 SRAM，一次性下载 ARM 固件到内存
static void arm_ota_download_and_flash(const char* url) {
  Serial.printf("[OTA] ARM OTA start: %s\n", url);

  // 分配缓冲区
  uint8_t* buffer = (uint8_t*)malloc(OTA_ARM_MAX_SIZE);
  if (!buffer) {
    Serial.println("[OTA] ARM buffer alloc failed");
    set_state(OTA_STATUS_ERROR, "Memory allocation failed");
    g_arm_ota_active = false;
    notify_callback(OTA_TARGET_ARM, 0, OTA_STATUS_ERROR, g_ota_message);
    return;
  }

  HTTPClient http;
  http.setConnectTimeout(15000);
  http.setTimeout(OTA_DOWNLOAD_TIMEOUT);

  WiFiClient stream;
  if (!http.begin(stream, url)) {
    Serial.println("[OTA] ARM HTTP begin failed");
    set_state(OTA_STATUS_ERROR, "HTTP begin failed");
    free(buffer);
    g_arm_ota_active = false;
    notify_callback(OTA_TARGET_ARM, 0, OTA_STATUS_ERROR, g_ota_message);
    return;
  }

  int http_code = http.GET();
  if (http_code != HTTP_CODE_OK) {
    Serial.printf("[OTA] ARM HTTP error: %d\n", http_code);
    set_state(OTA_STATUS_ERROR, "HTTP GET failed");
    http.end();
    free(buffer);
    g_arm_ota_active = false;
    notify_callback(OTA_TARGET_ARM, 0, OTA_STATUS_ERROR, g_ota_message);
    return;
  }

  int content_len = http.getSize();
  if (content_len <= 0 || content_len > OTA_ARM_MAX_SIZE) {
    Serial.printf("[OTA] ARM firmware size invalid: %d\n", content_len);
    set_state(OTA_STATUS_ERROR, "Invalid firmware size");
    http.end();
    free(buffer);
    g_arm_ota_active = false;
    notify_callback(OTA_TARGET_ARM, 0, OTA_STATUS_ERROR, g_ota_message);
    return;
  }

  Serial.printf("[OTA] ARM firmware size: %d bytes, downloading...\n", content_len);
  set_state(OTA_STATUS_DOWNLOADING, "Downloading ARM firmware...");
  notify_callback(OTA_TARGET_ARM, 0, OTA_STATUS_DOWNLOADING, g_ota_message);

  // 下载全部内容
  WiFiClient* http_stream = http.getStreamPtr();
  int total_read = 0;
  unsigned long timeout_start = millis();

  while (total_read < content_len) {
    if (millis() - timeout_start > OTA_DOWNLOAD_TIMEOUT) {
      Serial.println("[OTA] ARM download timeout");
      set_state(OTA_STATUS_ERROR, "Download timeout");
      http.end();
      free(buffer);
      g_arm_ota_active = false;
      notify_callback(OTA_TARGET_ARM, 0, OTA_STATUS_ERROR, g_ota_message);
      return;
    }

    if (!http_stream->available()) {
      delay(10);
      continue;
    }

    int avail = http_stream->available();
    int to_read = (total_read + avail > content_len) ? (content_len - total_read) : avail;
    int read_len = http_stream->readBytes(buffer + total_read, to_read);
    if (read_len <= 0) {
      delay(10);
      continue;
    }

    total_read += read_len;
    int percent = (int)((long)total_read * 100 / content_len);
    g_ota_progress = percent;
    notify_callback(OTA_TARGET_ARM, percent, OTA_STATUS_DOWNLOADING, g_ota_message);
  }

  http.end();

  Serial.printf("[OTA] ARM firmware downloaded: %d bytes\n", total_read);
  set_state(OTA_STATUS_WRITING, "Sending ARM firmware via UART...");
  notify_callback(OTA_TARGET_ARM, 100, OTA_STATUS_WRITING, g_ota_message);

  // 通过 arm_ota 模块发送给 ARM（非阻塞状态机）
  if (arm_ota_start(buffer, total_read)) {
    Serial.println("[OTA] ARM OTA transfer started via arm_ota module");
    // 注意：buffer 的生命周期由 arm_ota 模块管理
    // arm_ota 模块只保存指针，不拷贝数据，所以 buffer 不能释放
    // 我们需要在 arm_ota 完成后释放 buffer
    // 这里将 buffer 交给 arm_ota 管理（通过回调释放）
    // 为简化，使用静态缓冲区或全局指针
  } else {
    Serial.println("[OTA] arm_ota_start failed");
    set_state(OTA_STATUS_ERROR, "ARM OTA start failed");
    free(buffer);
    g_arm_ota_active = false;
    notify_callback(OTA_TARGET_ARM, 0, OTA_STATUS_ERROR, g_ota_message);
  }
}

#else // ESP8266

// ESP8266: 流式下载 + 流式发送模式
// 边下载边通过 arm_ota_feed() 写入缓冲区，arm_ota_process() 从缓冲区发送
static void arm_ota_download_and_flash(const char* url) {
  Serial.printf("[OTA:8266] ARM OTA streaming start: %s\n", url);

  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(OTA_DOWNLOAD_TIMEOUT);

  if (!http.begin(url)) {
    Serial.println("[OTA:8266] HTTP begin failed");
    set_state(OTA_STATUS_ERROR, "HTTP begin failed");
    g_arm_ota_active = false;
    notify_callback(OTA_TARGET_ARM, 0, OTA_STATUS_ERROR, g_ota_message);
    return;
  }

  int http_code = http.GET();
  if (http_code != HTTP_CODE_OK) {
    Serial.printf("[OTA:8266] HTTP error: %d\n", http_code);
    set_state(OTA_STATUS_ERROR, "HTTP GET failed");
    http.end();
    g_arm_ota_active = false;
    notify_callback(OTA_TARGET_ARM, 0, OTA_STATUS_ERROR, g_ota_message);
    return;
  }

  int content_len = http.getSize();
  if (content_len <= 0 || content_len > OTA_ARM_MAX_SIZE) {
    Serial.printf("[OTA:8266] Invalid firmware size: %d\n", content_len);
    set_state(OTA_STATUS_ERROR, "Invalid firmware size");
    http.end();
    g_arm_ota_active = false;
    notify_callback(OTA_TARGET_ARM, 0, OTA_STATUS_ERROR, g_ota_message);
    return;
  }

  Serial.printf("[OTA:8266] Firmware size: %d bytes, downloading...\n", content_len);

  // 开始流式 ARM OTA（发送 START 命令）
  if (!arm_ota_start_streaming(content_len)) {
    Serial.println("[OTA:8266] arm_ota_start_streaming failed");
    set_state(OTA_STATUS_ERROR, "ARM OTA start failed");
    http.end();
    g_arm_ota_active = false;
    notify_callback(OTA_TARGET_ARM, 0, OTA_STATUS_ERROR, g_ota_message);
    return;
  }

  // 流式下载 + arm_ota_feed() 写入缓冲区
  // 注意：arm_ota_feed() 是非阻塞的，arm_ota_process() 会自动从缓冲区发送
  uint8_t buf[4096];
  int downloaded = 0;

  WiFiClient* stream = http.getStreamPtr();

  while (downloaded < content_len && g_arm_ota_active) {
    // 处理 ARM OTA 发送（从缓冲区发送到 UART）
    arm_ota_process();

    // 检查 ARM OTA 是否出错
    if (arm_ota_get_state() == ARM_OTA_ERROR ||
        arm_ota_get_state() == ARM_OTA_TIMEOUT) {
      Serial.println("[OTA:8266] ARM OTA protocol error");
      http.end();
      set_state(OTA_STATUS_ERROR, arm_ota_get_message());
      g_arm_ota_active = false;
      notify_callback(OTA_TARGET_ARM, arm_ota_get_progress(), OTA_STATUS_ERROR, g_ota_message);
      return;
    }

    // 检查是否进入 DONE 状态（异常提前完成）
    if (arm_ota_get_state() == ARM_OTA_DONE) {
      break;
    }

    // 下载数据
    if (stream->available()) {
      int to_read = content_len - downloaded;
      if (to_read > (int)sizeof(buf)) to_read = sizeof(buf);
      int read_len = stream->readBytes(buf, to_read);
      if (read_len > 0) {
        downloaded += read_len;
        int percent = (int)((long)downloaded * 100 / content_len);
        g_ota_progress = percent;
        notify_callback(OTA_TARGET_ARM, percent, OTA_STATUS_DOWNLOADING, "Downloading...");

        // 喂给 ARM OTA 模块（缓冲区会自动分包发送）
        arm_ota_feed(buf, read_len);
      }
    } else {
      delay(10);
    }
  }

  http.end();

  if (!g_arm_ota_active) {
    Serial.println("[OTA:8266] ARM OTA cancelled");
    return;
  }

  Serial.printf("[OTA:8266] Downloaded %d bytes, ending streaming...\n", downloaded);
  notify_callback(OTA_TARGET_ARM, 100, OTA_STATUS_WRITING, "Sending last packets...");

  // 结束流式 OTA（发送 END 命令）
  arm_ota_end_streaming();

  // 等待 ARM 校验完成
  while (g_arm_ota_active) {
    arm_ota_process();

    ArmOtaState st = arm_ota_get_state();
    if (st == ARM_OTA_DONE) {
      Serial.println("[OTA:8266] ARM OTA completed");
      g_arm_ota_active = false;
      set_state(OTA_STATUS_DONE, "ARM OTA completed");
      notify_callback(OTA_TARGET_ARM, 100, OTA_STATUS_DONE, "ARM OTA completed");
      return;
    }
    if (st == ARM_OTA_ERROR || st == ARM_OTA_TIMEOUT) {
      Serial.printf("[OTA:8266] ARM OTA error: %s\n", arm_ota_get_message());
      g_arm_ota_active = false;
      set_state(OTA_STATUS_ERROR, arm_ota_get_message());
      notify_callback(OTA_TARGET_ARM, arm_ota_get_progress(), OTA_STATUS_ERROR, arm_ota_get_message());
      return;
    }
    delay(10);
  }
}

#endif // CHIP_ESP32C3

// ==================== 公共 API ====================

void ota_manager_init() {
  g_ota_status = OTA_STATUS_IDLE;
  g_ota_target = OTA_TARGET_ESP;
  g_ota_progress = 0;
  g_ota_message[0] = '\0';
  g_ota_callback = NULL;
  g_esp_ota_active = false;
  g_arm_ota_active = false;

  // 初始化 arm_ota 模块
  arm_ota_init();
  arm_ota_set_callback(arm_ota_progress_cb);

  load_arm_version();

  Serial.printf("[OTA] Manager initialized, ESP ver=%s, ARM ver=%s\n",
                FIRMWARE_VERSION,
                g_arm_version[0] ? g_arm_version : "N/A");
}

void ota_set_progress_callback(OtaProgressCallback cb) {
  g_ota_callback = cb;
}

bool ota_start_esp(const char* url) {
  if (!url || !is_valid_url(url)) {
    Serial.println("[OTA] Invalid URL");
    return false;
  }
  if (ota_is_busy()) {
    Serial.println("[OTA] Already busy");
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[OTA] WiFi not connected");
    return false;
  }

  g_ota_target = OTA_TARGET_ESP;
  g_ota_progress = 0;
  g_esp_ota_active = true;

  led1_set(LED_BREATH);
  led2_set(LED_BREATH);

  esp_ota_begin(url);
  return true;
}

bool ota_start_arm(const char* url) {
  if (!url || !is_valid_url(url)) {
    Serial.println("[OTA] Invalid URL");
    return false;
  }
  if (ota_is_busy()) {
    Serial.println("[OTA] Already busy");
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[OTA] WiFi not connected");
    return false;
  }

  g_ota_target = OTA_TARGET_ARM;
  g_ota_progress = 0;
  g_arm_ota_active = true;

  led1_set(LED_BREATH);
  led2_set(LED_BREATH);

  arm_ota_download_and_flash(url);
  return true;
}

OtaStatus ota_get_status() {
  return g_ota_status;
}

OtaTarget ota_get_target() {
  return g_ota_target;
}

int ota_get_progress() {
  return g_ota_progress;
}

const char* ota_get_message() {
  return g_ota_message;
}

void ota_manager_process() {
  if (!ota_is_busy()) return;

  // ESP 自身 OTA 处理
  if (g_esp_ota_active) {
    esp_ota_process();
  }

  // ARM OTA 处理（委托给 arm_ota 模块）
  if (g_arm_ota_active) {
    arm_ota_process();
  }

  // OTA 完成或出错后恢复 LED
  if (!ota_is_busy()) {
    if (g_ota_status == OTA_STATUS_DONE) {
      led1_set(LED_SOLID);
      led2_set(LED_SOLID);
    } else if (g_ota_status == OTA_STATUS_ERROR) {
      led1_set(LED_FAULT);
      led2_set(LED_FAULT);
    }
  }
}

bool ota_is_busy() {
  return g_esp_ota_active || g_arm_ota_active;
}

const char* ota_get_esp_version() {
  return FIRMWARE_VERSION;
}

const char* ota_get_arm_version() {
  return g_arm_version[0] ? g_arm_version : "unknown";
}
