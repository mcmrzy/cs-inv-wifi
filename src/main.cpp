/**
 * ================================================================
 *  光伏逆变器 WiFi 通讯模块 — Arduino 版本
 *  支持: ESP32-C3
 *
 *  职责: 桥接 ARM MCU (自定义 UART 协议) ←→ 云端/APP (MQTT)
 *
 *  通信协议:
 *    - ESP32 ↔ ARM: 自定义帧协议 (uart_protocol) @ 115200
 *      ARM 通过 CMD_PUBLISH 发送数据，ESP32 透传到 MQTT
 *    - ESP32 ↔ MQTT: 发布 JSON 格式逆变器数据 (inverter_data 定义)
 *      MQTT 控制命令透传到 ARM
 *
 *  开机行为:
 *    1. 有 WiFi 凭据 → 直接连 WiFi → 连 MQTT
 *    2. 无 WiFi 凭据 → 广播 AP 热点 CS_INV_xxxxxx
 * ================================================================
 */

#include "config.h"
#include "nvs_storage.h"
#include "uart_protocol.h"
#include "led_status.h"
#include "tcp_provision.h"
#include "mqtt_handler.h"
#include "mqtt_ota_handler.h"
#include "sn_encoder.h"
#include "ota_manager.h"
#include "arm_ota.h"
#include "offline_cache.h"
#include "cs_protocol_defs.h"
#include "ota_result.h"
#include <esp_mac.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// min 宏定义
#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif

#ifdef CHIP_ESP32C3
  #include <WiFi.h>
  #include "esp_task_wdt.h"
  #include "BleProvisionManager.h"
#else
  #include <ESP8266WiFi.h>
#endif

#include <string.h>

// ==================== 全局状态 ====================
static SystemState g_sys_state = STATE_INIT;

// 设备 SN 和 Key 在 mqtt_handler.cpp 中定义
extern char g_device_sn[SN_MAX_LEN];
extern char g_device_key[65];

static char g_wifi_ssid[33] = {0};
static char g_wifi_pass[65] = {0};
static unsigned long g_wifi_connect_start = 0;
static int g_wifi_reconnect_attempts = 0;
static unsigned long g_wifi_reconnect_delay = 1000;
static bool g_wifi_provisioning = false;  // BLE 配网触发的连接，快速失败

// g_last_heartbeat 已移除：status 不再周期性发送
static unsigned long g_last_uart_heartbeat = 0;
static unsigned long g_last_led_tick = 0;
static unsigned long g_last_debug = 0;

static bool g_btn_pressed = false;
static unsigned long g_btn_press_start = 0;

// ==================== UART PUBLISH 队列 ====================
// mqtt_publish(TLS) 阻塞时间长，不能在 UART 回调中调用，否则 UART 缓冲区溢出
#define PUB_QUEUE_SIZE 16
#define PUB_TOPIC_LEN 64
#define PUB_PAYLOAD_LEN (FRAME_DATA_MAX - 80)
struct PubQueueItem {
  char topic[PUB_TOPIC_LEN];
  char payload[PUB_PAYLOAD_LEN];
  bool used;
};
static PubQueueItem g_pub_queue[PUB_QUEUE_SIZE] = {};
static int g_pub_queue_head = 0;
static int g_pub_queue_tail = 0;

#define RECENT_TASK_COUNT 8
struct RecentTask {
  char task_id[64];
  char response[FRAME_DATA_MAX];
};
static RecentTask g_recent_tasks[RECENT_TASK_COUNT] = {};
static uint8_t g_recent_task_next = 0;

// ==================== 前向声明 ====================
static void init_wifi_sta();
static void state_ap_provision();
static void state_sta_connecting();
static void state_mqtt_connecting();
static void state_running();
static void on_uart_frame(uint8_t cmd, const uint8_t* data, uint16_t len);
static void on_provision_done(const char* ssid, const char* password);
static void on_mqtt_message(const char* topic, const char* payload, unsigned int len);

// ==================== MQTT OTA 处理（已移至 mqtt_ota_handler.cpp） ====================
// 参考 mqtt_ota_handler.h 中的 API
static void check_button();
static void build_ap_ssid(char* out, size_t out_len);

// CMD_PUBLISH 辅助函数
static bool json_extract_str(const char* json, const char* key, char* out, size_t max_len);
static bool replace_firmware_version(const char* payload, char* out, size_t max_len, const char* version);
static bool handle_sn_update(const char* new_sn);
static RecentTask* find_task(const char* task_id);
static void remember_task(const char* task_id, const char* response);
static void update_task_response(const char* json);
static void load_persisted_task_response();
static void clear_persisted_task_response();

// ==================== OTA 进度回调（用于传统 OTA 管理器） ====================
static void ota_progress_mqtt_callback(OtaTarget target, int percent, OtaStatus status, const char* message) {
  // 这个函数留作备用，给传统 OTA Manager 使用
  // MQTT OTA 处理通过 mqtt_ota_handler 直接上报
}

// ==================== 前向声明（结束） ====================

// ==================== 队列辅助函数 ====================
static bool pub_queue_push(const char* topic, const char* payload) {
  if (!topic || !payload || strlen(topic) >= PUB_TOPIC_LEN ||
      strlen(payload) >= PUB_PAYLOAD_LEN) {
    Serial.println("[PUB-Q] Topic or payload too large");
    return false;
  }
  int next = (g_pub_queue_head + 1) % PUB_QUEUE_SIZE;
  if (next == g_pub_queue_tail) {
    Serial.println("[PUB-Q] Queue full, dropping message");
    return false;
  }
  strncpy(g_pub_queue[g_pub_queue_head].topic, topic, PUB_TOPIC_LEN - 1);
  strncpy(g_pub_queue[g_pub_queue_head].payload, payload, PUB_PAYLOAD_LEN - 1);
  g_pub_queue[g_pub_queue_head].topic[PUB_TOPIC_LEN - 1] = '\0';
  g_pub_queue[g_pub_queue_head].payload[PUB_PAYLOAD_LEN - 1] = '\0';
  g_pub_queue[g_pub_queue_head].used = true;
  g_pub_queue_head = next;
  return true;
}

static void pub_queue_process() {
  if (g_pub_queue_tail == g_pub_queue_head) return; // 空队列
  PubQueueItem* item = &g_pub_queue[g_pub_queue_tail];
  if (item->used) {
    int result = mqtt_publish(item->topic, item->payload, strlen(item->payload), 1, 0);
    if (result != 0 && result != -1) {
      Serial.printf("[PUB-Q] Publish failed (%d), retaining %s\n", result, item->topic);
      return;
    }
    Serial.printf("[PUB-Q] Delivered or cached %s (%d bytes)\n",
                  item->topic, strlen(item->payload));
    item->used = false;
  }
  g_pub_queue_tail = (g_pub_queue_tail + 1) % PUB_QUEUE_SIZE;
}

static RecentTask* find_task(const char* task_id) {
  if (!task_id || !task_id[0]) return nullptr;
  for (uint8_t i = 0; i < RECENT_TASK_COUNT; ++i) {
    if (strcmp(g_recent_tasks[i].task_id, task_id) == 0) return &g_recent_tasks[i];
  }
  return nullptr;
}

static void remember_task(const char* task_id, const char* response) {
  if (!task_id || !task_id[0] || !response) return;
  RecentTask* entry = &g_recent_tasks[g_recent_task_next];
  strncpy(entry->task_id, task_id, sizeof(entry->task_id) - 1);
  entry->task_id[sizeof(entry->task_id) - 1] = '\0';
  strncpy(entry->response, response, sizeof(entry->response) - 1);
  entry->response[sizeof(entry->response) - 1] = '\0';
  g_recent_task_next = (g_recent_task_next + 1) % RECENT_TASK_COUNT;
}

static void update_task_response(const char* json) {
  if (!json) return;
  char task_id[64] = {0};
  if (!json_extract_str(json, "\"task_id\"", task_id, sizeof(task_id))) return;
  RecentTask* entry = find_task(task_id);
  if (!entry) return;
  strncpy(entry->response, json, sizeof(entry->response) - 1);
  entry->response[sizeof(entry->response) - 1] = '\0';
  char stage[24] = {0};
  if (json_extract_str(json, "\"stage\"", stage, sizeof(stage)) &&
      strcmp(stage, PROTO_CMD_STAGE_DONE) == 0) {
    Preferences prefs;
    prefs.begin("cmd_idem", false);
    uint8_t slot = prefs.getUChar("next", 0) % 4;
    char task_key[4];
    char response_key[4];
    snprintf(task_key, sizeof(task_key), "t%u", slot);
    snprintf(response_key, sizeof(response_key), "r%u", slot);
    prefs.putString(task_key, task_id);
    prefs.putString(response_key, entry->response);
    prefs.putUChar("next", (slot + 1) % 4);
    prefs.end();
  }
}

static void load_persisted_task_response() {
  Preferences prefs;
  prefs.begin("cmd_idem", true);
  for (uint8_t slot = 0; slot < 4; ++slot) {
    char task_key[4];
    char response_key[4];
    snprintf(task_key, sizeof(task_key), "t%u", slot);
    snprintf(response_key, sizeof(response_key), "r%u", slot);
    String task = prefs.getString(task_key, "");
    String response = prefs.getString(response_key, "");
    if (task.length() && response.length()) remember_task(task.c_str(), response.c_str());
  }
  prefs.end();
}

static void clear_persisted_task_response() {
  Preferences prefs;
  prefs.begin("cmd_idem", false);
  prefs.clear();
  prefs.end();
}

// ==================== CMD_PUBLISH 辅助函数 ====================

// 从 JSON 中提取字符串字段的值
static bool json_extract_str(const char* json, const char* key, char* out, size_t max_len) {
  if (!json || !key || !out || max_len == 0) {
    return false;
  }
  
  char* p = strstr((char*)json, key);
  if (!p) {
    return false;
  }
  
  p = strchr(p + strlen(key), ':');
  if (!p) {
    return false;
  }
  
  p = strchr(p + 1, '"');
  if (!p) {
    return false;
  }
  
  p++;  // 跳过开始的引号
  size_t i = 0;
  while (*p && *p != '"' && i < max_len - 1) {
    // 处理转义字符
    if (*p == '\\' && *(p + 1)) {
      p++;
    }
    out[i++] = *p++;
  }
  out[i] = '\0';
  
  return i > 0;  // 至少提取到一个字符才返回 true
}

// 替换 payload 中的 firmware_esp 版本号
static bool replace_firmware_version(const char* payload, char* out, size_t max_len, const char* version) {
  if (!payload || !out || !version || max_len == 0) {
    return false;
  }
  
  const char* esp_pos = strstr(payload, "\"firmware_esp\"");
  if (!esp_pos) {
    // 没有找到版本号字段，直接复制原 payload
    strncpy(out, payload, max_len - 1);
    out[max_len - 1] = '\0';
    return false;
  }
  
  const char* ver_start = strchr(esp_pos + 14, ':');
  if (!ver_start) {
    return false;
  }
  
  ver_start = strchr(ver_start + 1, '"');
  if (!ver_start) {
    return false;
  }
  
  const char* ver_end = strchr(ver_start + 1, '"');
  if (!ver_end) {
    return false;
  }
  
  // 计算各部分长度
  size_t prefix_len = ver_start + 1 - payload;
  size_t suffix_len = strlen(ver_end);
  size_t ver_len = strlen(version);
  
  // 检查缓冲区大小
  if (prefix_len + ver_len + suffix_len >= max_len) {
    Serial.println("[UART] Payload too large after version replacement");
    return false;
  }
  
  // 构建新的 payload
  memcpy(out, payload, prefix_len);
  memcpy(out + prefix_len, version, ver_len);
  strcpy(out + prefix_len + ver_len, ver_end);
  
  return true;
}

// 处理 SN 更新逻辑
// 返回值: true = 可以继续发布, false = SN 校验失败，应拒绝发布
static bool handle_sn_update(const char* new_sn) {
  if (!new_sn || strlen(new_sn) == 0) {
    return true;  // 无 SN 字段，不影响发布
  }
  
  // SN 未变化，无需处理，继续发布
  if (strcmp(new_sn, g_device_sn) == 0) {
    return true;
  }
  
  // SN 格式校验（失败则拒绝发布）
  if (!sn_validate(new_sn)) {
    Serial.printf("[UART] SN REJECTED (invalid): %s\n", new_sn);
    return false;
  }
  
  // 保存 SN 到存储
  storage_set_sn(new_sn);
  strncpy(g_device_sn, new_sn, sizeof(g_device_sn) - 1);
  Serial.printf("[UART] SN updated: %s\n", new_sn);
  
  // 重连 MQTT（使用新的 SN）
  mqtt_disconnect();
  mqtt_init(on_mqtt_message);
  if (g_sys_state == STATE_RUNNING) {
    g_sys_state = STATE_MQTT_CONNECTING;
  }
  
  // 更新 AP SSID
  char ap_ssid[32];
  build_ap_ssid(ap_ssid, sizeof(ap_ssid));
  WiFi.softAP(ap_ssid, NULL, 1, 0, 1);
  
  return true;
}

// ==================== BLE Provisioning ====================
#ifdef CHIP_ESP32C3
static BleProvisionManager g_bleprov;

static void on_bleprov_done(const char* ssid, const char* password) {
  log_i("[BLE-PROV] callback: SSID=%s", ssid);
  on_provision_done(ssid, password);
}
#endif

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  delay(3000);
  Serial.println("\n\n========================================");
  Serial.println(" INVERTER WiFi MODULE (Arduino)");
  Serial.println(" ESP32-C3 + Custom UART + MQTT");
  Serial.println("========================================");

  storage_init();
  load_persisted_task_response();
  uart_protocol_init();
  led_init();
  offline_cache_init();  // 初始化离线缓存

  pinMode(PIN_BTN, INPUT_PULLUP);
  Serial.printf("[HW] BTN=IO%d\n", PIN_BTN);

  uart_protocol_set_callback(on_uart_frame);
  tcp_provision_set_callback(on_provision_done);

  if (storage_has_sn()) {
    storage_get_sn(g_device_sn, sizeof(g_device_sn));
    storage_get_key(g_device_key, sizeof(g_device_key));

    // 校验已存储的 SN
    if (sn_validate(g_device_sn)) {
      SNFields sf;
      sn_parse(g_device_sn, &sf);
      char readable[32];
      sn_printable(g_device_sn, readable, sizeof(readable));
      Serial.printf("[SYS] SN loaded (valid): %s\n", readable);
      Serial.printf("[SYS]   Mfr=%s Country=%s Cust=%s Date=%s Seq=%s\n",
                    sf.manufacturer, sf.country, sf.customer,
                    sf.date_code, sf.sequence);
    } else {
      Serial.printf("[SYS] SN loaded (INVALID format): %s\n", g_device_sn);
    }
  } else {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(g_device_sn, sizeof(g_device_sn),
             "MAC%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    Serial.printf("[SYS] No SN -> using MAC: %s\n", g_device_sn);
  }

  mqtt_init(on_mqtt_message);

  // 初始化 OTA 相关模块
  ota_manager_init();
  ota_set_progress_callback(ota_progress_mqtt_callback);
  mqtt_ota_init();  // 初始化 MQTT OTA 处理

  // BLE 先初始化（必须在 WiFi 连接之前，避免 ESP32-C3 单射频前端资源冲突）
#ifdef CHIP_ESP32C3
  g_bleprov.setCallback(on_bleprov_done);
  g_bleprov.start(g_device_sn, FIRMWARE_VERSION);
#endif

  // 优先从 NVS 加载已保存的 WiFi 配网信息
  if (storage_has_wifi_config()) {
    storage_get_wifi(g_wifi_ssid, sizeof(g_wifi_ssid), g_wifi_pass, sizeof(g_wifi_pass));
    Serial.printf("[SYS] Loaded WiFi from NVS: %s\n", g_wifi_ssid);
    init_wifi_sta();
    g_sys_state = STATE_STA_CONNECTING;
    led1_set(LED_FAST_BLINK);
    led2_set(LED_OFF);
  } else {
    // 没有保存的 WiFi 信息，进入 AP 配网模式
    Serial.println("[SYS] No saved WiFi -> AP Provision mode");
    char ap_ssid[32];
    build_ap_ssid(ap_ssid, sizeof(ap_ssid));
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(200);
    WiFi.mode(WIFI_AP_STA);  // AP_STA 模式：BLE 配网 + AP 热点并存
    WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
    WiFi.softAP(ap_ssid, NULL, 1, 0, 1);
    Serial.printf("[AP] softAP(%s) IP=%s\n", ap_ssid, WiFi.softAPIP().toString().c_str());
    g_sys_state = STATE_AP_PROVISION;
    led1_set(LED_SLOW_BLINK);   // LED1 慢闪=AP配网模式
    led2_set(LED_OFF);
  }
  tcp_provision_init();

  configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org");
  g_last_debug = millis();
  Serial.println("[SYS] Setup done");
}

// ==================== LOOP ====================
void loop() {
  unsigned long now = millis();

  // 0. NTP 同步检测：首次同步后保存 epoch 到 NVS
  {
    static bool ntp_saved = false;
    if (!ntp_saved && time(nullptr) > 1000000000) {
      ntp_saved = true;
      offline_cache_save_epoch();
      Serial.printf("[SYS] NTP synced, epoch saved: %lu\n", (unsigned long)time(nullptr));
    }
  }

  // 1. UART 数据处理（最优先）
  uart_protocol_process();

  // 1.5 处理 UART 发布队列（mqtt_publish 阻塞，不能在 UART 回调中调用）
  pub_queue_process();

  // 2. 按键检测
  check_button();

  // 3. LED 刷新 (~50ms)
  if (now - g_last_led_tick >= 50) {
    g_last_led_tick = now;
    led_tick();
  }

  // 4. 状态机
  switch (g_sys_state) {
    case STATE_INIT:
      break;
    case STATE_AP_PROVISION:
      tcp_provision_process();
      break;
    case STATE_STA_CONNECTING:
      state_sta_connecting();
      tcp_provision_process();
      break;
    case STATE_MQTT_CONNECTING:
      state_mqtt_connecting();
      tcp_provision_process();
      break;
    case STATE_RUNNING:
      state_running();
      tcp_provision_process();
      mqtt_ota_process();  // 处理 MQTT OTA
      break;
    case STATE_OTA:
      ota_manager_process();
      tcp_provision_process();
      break;
  }

  // BLE 始终处理（持续广播）
#ifdef CHIP_ESP32C3
  g_bleprov.process();
#endif

  // 5. 定期调试输出（每 60 秒）
  if (now - g_last_debug >= 60000) {
    g_last_debug = now;
    int cached = offline_cache_count();
    Serial.printf("[DBG] State=%d, WiFi=%d, Heap=%d, Cached=%d\n",
                  g_sys_state, WiFi.status(), ESP.getFreeHeap(), cached);
  }

  // 6. UART 心跳 (无论MQTT状态，定期发送给ARM)
  if (now - g_last_uart_heartbeat >= 10000) {  // 10秒一次
    g_last_uart_heartbeat = now;
    uart_send_frame(CMD_HEARTBEAT, nullptr, 0);
    Serial.println("[UART] HEARTBEAT sent");
  }

  // 7. 喂狗
  #ifdef CHIP_ESP32C3
  esp_task_wdt_reset();
  #endif

  delay(2);
}

// ==================== 构建 AP SSID ====================
static void build_ap_ssid(char* out, size_t out_len) {
  char custom_ssid[AP_SSID_MAX_LEN];
  if (storage_get_ap_ssid(custom_ssid, sizeof(custom_ssid))) {
    size_t clen = strlen(custom_ssid);
    bool valid = (clen >= 2 && clen <= 31);
    if (valid) {
      for (size_t i = 0; i < clen; i++) {
        if (custom_ssid[i] < 0x20 || custom_ssid[i] > 0x7E) { valid = false; break; }
      }
    }
    if (valid) {
      strncpy(out, custom_ssid, out_len - 1);
      out[out_len - 1] = '\0';
      return;
    } else {
      storage_clear_ap_ssid();
    }
  }

  // 默认热点名: CS_INV_ + 完整 SN 号
  size_t sn_len = strlen(g_device_sn);
  size_t max_sn = (out_len > strlen(AP_SSID_PREFIX) + 1) 
                  ? (out_len - strlen(AP_SSID_PREFIX) - 1) : 0;
  size_t copy_len = (sn_len < max_sn) ? sn_len : max_sn;
  memcpy(out, AP_SSID_PREFIX, strlen(AP_SSID_PREFIX));
  memcpy(out + strlen(AP_SSID_PREFIX), g_device_sn, copy_len);
  out[strlen(AP_SSID_PREFIX) + copy_len] = '\0';
  // 将 SN 部分转为大写
  for (char* p = out + strlen(AP_SSID_PREFIX); *p; p++) {
    if (*p >= 'a' && *p <= 'z') *p = *p - 'a' + 'A';
  }
}

// ==================== WiFi STA ====================
static void init_wifi_sta() {
  char ap_ssid[32];
  build_ap_ssid(ap_ssid, sizeof(ap_ssid));
  Serial.printf("[WIFI] init_wifi_sta: ssid='%s', pass_len=%d\n", g_wifi_ssid, strlen(g_wifi_pass));
  
  // 只断开 STA 连接，保持 AP 模式运行（确保配网热点始终可用）
  WiFi.disconnect(false, false);  // 仅断开 STA，不影响 AP
  delay(50);
  
  // 确保处于 AP_STA 模式
  if (WiFi.getMode() != WIFI_AP_STA) {
    WiFi.mode(WIFI_AP_STA);
    delay(100);
  }
  
  // WiFi 和蓝牙共存时必须启用 modem sleep
  WiFi.setSleep(true);// WiFi 和蓝牙共存时必须启用 modem sleep
  WiFi.setSleep(true);

  // 重新配置 AP（如果之前未配置）
  if (WiFi.softAPIP() != IPAddress(192, 168, 4, 1)) {
    WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
    WiFi.softAP(ap_ssid, NULL, 1, 0, 1);
    Serial.printf("[AP] softAP(%s) IP=%s\n", ap_ssid, WiFi.softAPIP().toString().c_str());
  }
  
  WiFi.begin(g_wifi_ssid, g_wifi_pass);
  g_wifi_connect_start = millis();
  g_wifi_reconnect_attempts = 0;
  Serial.printf("[WIFI] Connecting to %s...\n", g_wifi_ssid);
}

static unsigned long g_wifi_retry_next = 0;  // 下次重试时间点

static void state_sta_connecting() {
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WIFI] Connected! STA_IP=%s AP_IP=%s\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.softAPIP().toString().c_str());
    g_wifi_reconnect_attempts = 0;
    g_wifi_reconnect_delay = 1000;
    g_wifi_retry_next = 0;
    g_wifi_provisioning = false;

#ifdef CHIP_ESP32C3
    g_bleprov.notifyStatus("connected");
#endif

    g_sys_state = STATE_MQTT_CONNECTING;
    led1_set(LED_SOLID);       // LED1 常亮=WiFi已连
    led2_set(LED_SLOW_BLINK);  // LED2 慢闪=MQTT连接中
    // LWT 在 mqtt_connect() 中自动设置
    return;
  }

  unsigned long now = millis();

  // 首次进入或连接超时（15秒无结果）
  if (g_wifi_retry_next == 0 && now - g_wifi_connect_start > 15000) {
    g_wifi_reconnect_attempts++;
    Serial.printf("[WIFI] Retry %d\n", g_wifi_reconnect_attempts);
    int max_retries = g_wifi_provisioning ? 1 : 10;
    if (g_wifi_reconnect_attempts >= max_retries) {
      Serial.printf("[WIFI] Max retries (%d) -> AP mode\n", max_retries);
#ifdef CHIP_ESP32C3
      // 通知 BLE 配网端连接失败
      wl_status_t st = WiFi.status();
      if (st == WL_NO_SSID_AVAIL) {
        g_bleprov.notifyStatus("not_found");
      } else {
        g_bleprov.notifyStatus("failed");
      }
#endif
      g_wifi_provisioning = false;
      // 不删除凭据：暂时连不上不代表凭据错了，下次重启还能自动重试
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      delay(200);
      WiFi.mode(WIFI_AP_STA);  // AP_STA: BLE 配网 + AP 热点并存
      WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
      char ap_ssid[32];
      build_ap_ssid(ap_ssid, sizeof(ap_ssid));
      WiFi.softAP(ap_ssid, NULL, 1, 0, 1);
      Serial.printf("[AP] softAP(%s) IP=%s\n", ap_ssid, WiFi.softAPIP().toString().c_str());
      g_sys_state = STATE_AP_PROVISION;
      led1_set(LED_SLOW_BLINK);   // LED1 慢闪=AP配网
      led2_set(LED_OFF);
      tcp_provision_init();
      // BLE 已在 setup() 中启动，无需重复启动
      g_wifi_retry_next = 0;
      return;
    }
    // 非阻塞：设置下次重试时间点，立即返回继续处理 HTTP/AP
    g_wifi_retry_next = now + g_wifi_reconnect_delay;
    Serial.printf("[WIFI] Waiting %lu ms before retry...\n", g_wifi_reconnect_delay);
    g_wifi_reconnect_delay = min(g_wifi_reconnect_delay * 2, 60000UL);
    return;
  }

  // 等待重试时间到达（非阻塞，立即返回）
  if (g_wifi_retry_next > 0 && now < g_wifi_retry_next) {
    return;
  }

  // 执行重试连接
  if (g_wifi_retry_next > 0 && now >= g_wifi_retry_next) {
    g_wifi_retry_next = 0;
    WiFi.disconnect(false);
    WiFi.begin(g_wifi_ssid, g_wifi_pass);
    g_wifi_connect_start = millis();
  }
}

// ==================== MQTT 连接 ====================
static void state_mqtt_connecting() {
  if (WiFi.status() != WL_CONNECTED) {
    init_wifi_sta();
    g_sys_state = STATE_STA_CONNECTING;
    led1_set(LED_FAST_BLINK);   // LED1 快闪=WiFi重连
    led2_set(LED_OFF);
    return;
  }
  if (mqtt_is_connected()) {
    // 订阅已在 mqtt_connect() 中完成
    Serial.println("[SYS] >>> RUNNING <<<");
    g_sys_state = STATE_RUNNING;
    led1_set(LED_SOLID);        // 一切正常!
    led2_set(LED_SOLID);
    return;
  }
  // 只在首次进入时调用一次 mqtt_connect，
  // 后续重连交给 mqtt_loop() 内部的退避逻辑
  mqtt_loop();
}

// ==================== 运行状态 ====================
static void state_running() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WIFI] Lost!");
    led1_set(LED_FAST_BLINK);   // LED1 快闪=WiFi丢失，重连中
    led2_set(LED_OFF);
    mqtt_disconnect();
    init_wifi_sta();
    g_sys_state = STATE_STA_CONNECTING;
    return;
  }
  if (!mqtt_is_connected()) {
    led1_set(LED_SOLID);        // LED1 常亮=WiFi正常
    led2_set(LED_FAULT);        // LED2 快闪=MQTT故障
    g_sys_state = STATE_MQTT_CONNECTING;
    return;
  }
  mqtt_loop();

  // status 仅在上线/主动下线/LWT 时发布（MQTT keepalive 维护连接状态）
}

// ==================== UART 帧处理 (ARM → ESP32) ====================
static void on_uart_frame(uint8_t cmd, const uint8_t* data, uint16_t len) {
  switch (cmd) {
    case CMD_SET_BROKER: {
      char buf[128] = {0};
      memcpy(buf, data, min((size_t)len, sizeof(buf) - 1));
      char* colon = strchr(buf, ':');
      if (colon) {
        *colon = '\0';
        storage_set_mqtt(buf, (uint16_t)atoi(colon + 1));
        Serial.printf("[UART] Broker: %s:%s\n", buf, colon + 1);
        mqtt_disconnect();
        mqtt_init(on_mqtt_message);
        g_sys_state = STATE_MQTT_CONNECTING;
      }
      uart_send_ack();
      break;
    }

    case CMD_PUBLISH: {
      // ARM 发送 JSON: {"topic":"xxx","payload":"yyy","sn":"xxx"}
      // ESP32 发布到 MQTT: cs_inv/{sn}/{topic}
      char json[FRAME_DATA_MAX];
      size_t copy_len = (len < sizeof(json) - 1) ? len : sizeof(json) - 1;
      memcpy(json, data, copy_len);
      json[copy_len] = '\0';
      
      // 提取 JSON 字段
      char topic[64] = {0};
      char payload[FRAME_DATA_MAX - 80] = {0};
      char sn[SN_MAX_LEN] = {0};
      
      json_extract_str(json, "\"topic\"", topic, sizeof(topic));
      json_extract_str(json, "\"payload\"", payload, sizeof(payload));
      json_extract_str(json, "\"sn\"", sn, sizeof(sn));
      
      // 处理 SN 更新（如果提供了新 SN）
      if (strlen(sn) > 0) {
        if (!handle_sn_update(sn)) {
          // SN 校验失败，拒绝发布
          uart_send_nack();
          return;
        }
      }
      
      // 处理消息发布
      if (strlen(topic) > 0) {
        // 替换 payload 中的 firmware_esp 为实际版本号
        char final_payload[FRAME_DATA_MAX - 80];
        const char* esp_ver = ota_get_esp_version();
        const char* send_payload = payload;
        
        if (replace_firmware_version(payload, final_payload, sizeof(final_payload), esp_ver)) {
          send_payload = final_payload;
          Serial.printf("[UART] Replaced firmware_esp with %s\n", esp_ver);
        }
        
        // While offline, persist before ACK so a reset cannot lose an accepted
        // ARM message. Online TLS publishing remains deferred to the RAM queue.
        if (!mqtt_is_connected()) {
          int result = mqtt_publish(topic, send_payload, strlen(send_payload), 1, 0);
          if (result == -1) {
            uart_send_ack();
            Serial.printf("[UART] PUBLISH persisted offline -> %s\n", topic);
          } else {
            uart_send_nack();
            Serial.printf("[UART] PUBLISH cache failed -> %s (%d)\n", topic, result);
          }
        } else if (pub_queue_push(topic, send_payload)) {
          uart_send_ack();
          led_signal_data();    // LED2 短暂闪一下表示有数据活动
          Serial.printf("[UART] PUBLISH queued -> cs_inv/%s/%s (%d bytes)\n",
                        g_device_sn, topic, strlen(send_payload));
        } else {
          uart_send_nack();
        }
      } else {
        Serial.println("[UART] PUBLISH error: topic empty");
        uart_send_nack();
      }
      break;
    }

    case CMD_SET_SN: {
      char sn[SN_MAX_LEN] = {0};
      memcpy(sn, data, min((size_t)len, sizeof(sn) - 1));

      // ---- SN 格式校验 (CS-SN-STD-001 V1.1) ----
      if (!sn_validate(sn)) {
        Serial.printf("[UART] SN REJECTED (invalid): %s\n", sn);
        uart_send_nack();
        break;
      }

      // 输出解析信息
      SNFields sf;
      if (sn_parse(sn, &sf)) {
        char readable[32];
        sn_printable(sn, readable, sizeof(readable));
        int year = sn_year_code_decode(sf.date_code[0]);
        char month = sf.date_code[1];
        Serial.printf("[UART] SN validated: %s | Mfr=%s Country=%s Cust=%s "
                      "Year=%d Month=%c Seq=%s Check=%c\n",
                      readable, sf.manufacturer, sf.country, sf.customer,
                      year, month, sf.sequence, sf.check_digit);
      }

      // 只有 SN 真正变化时才需要重启（SN 变更影响 MQTT client_id 和主题）
      bool changed = (strcmp(g_device_sn, sn) != 0);
      storage_set_sn(sn);
      strncpy(g_device_sn, sn, sizeof(g_device_sn) - 1);
      uart_send_ack();
      if (changed) {
        Serial.printf("[UART] SN changed to %s, restarting...\n", sn);
        delay(200);
        ESP.restart();
      } else {
        Serial.printf("[UART] SN=%s (no change)\n", sn);
      }
      break;
    }

    case CMD_SET_KEY: {
      char key[65] = {0};
      memcpy(key, data, min((size_t)len, sizeof(key) - 1));
      storage_set_key(key);
      strncpy(g_device_key, key, sizeof(g_device_key) - 1);
      uart_send_ack();
      break;
    }

    case CMD_SET_AP_SSID: {
      char ap_ssid[AP_SSID_MAX_LEN] = {0};
      memcpy(ap_ssid, data, min((size_t)len, sizeof(ap_ssid) - 1));
      if (strlen(ap_ssid) == 0) {
        storage_clear_ap_ssid();
        Serial.println("[UART] AP SSID reset to default");
      } else {
        storage_set_ap_ssid(ap_ssid);
        Serial.printf("[UART] AP SSID set to: %s\n", ap_ssid);
      }
      uart_send_ack();
      delay(100);
      ESP.restart();
      break;
    }

    case CMD_FACTORY_RESET:
      offline_cache_factory_reset();
      clear_persisted_task_response();
      ota_result_clear();
      storage_factory_reset();
      uart_send_ack();
      delay(100);
      ESP.restart();
      break;

    // ARM OTA 相关命令（转发给 arm_ota 模块处理）
    case CMD_ARM_OTA_ACK:
    case CMD_ARM_OTA_NACK:
    case CMD_ARM_OTA_INFO:
      arm_ota_handle_uart(cmd, data, len);
      break;

    // ARM 回复的 ACK/NACK，忽略不处理
    case CMD_ACK:
    case CMD_NACK:
      break;

    // ARM 命令执行结果，转发到 MQTT
    case CMD_COMMAND_SEND: {
      char json[FRAME_DATA_MAX];
      size_t copy_len = (len < sizeof(json) - 1) ? len : sizeof(json) - 1;
      memcpy(json, data, copy_len);
      json[copy_len] = '\0';
      update_task_response(json);
      mqtt_publish_envelope(PROTO_TOPIC_CMD_RESP, json, copy_len);
      Serial.printf("[ARM→MQTT] cmd_result: %s\n", json);
      uart_send_ack();
      break;
    }

    default:
      uart_send_nack();
      break;
  }
}

// ==================== 配网回调 ====================
static void on_provision_done(const char* ssid, const char* password) {
  Serial.printf("[PROV] Got: %s, pass_len=%d\n", ssid, strlen(password));
  // BLE 持续广播，不停止
  storage_set_wifi(ssid, password);
  strncpy(g_wifi_ssid, ssid, sizeof(g_wifi_ssid) - 1);
  strncpy(g_wifi_pass, password, sizeof(g_wifi_pass) - 1);
  g_wifi_provisioning = true;  // 标记为配网触发，快速失败
  init_wifi_sta();
  g_sys_state = STATE_STA_CONNECTING;
  led1_set(LED_FAST_BLINK);   // LED1 快闪=配网成功，WiFi连接中
  led2_set(LED_OFF);
  Serial.printf("[PROV] AP kept running, BLE continues advertising, connecting STA to %s\n", ssid);
}

// ==================== MQTT 消息回调 (云端 → ESP32 → ARM) ====================
static void on_mqtt_message(const char* topic, const char* payload, unsigned int len) {
  // 查找 topic 中最后一个 / 后的子主题
  const char* sub = strrchr(topic, '/');
  if (sub) sub++; else sub = topic;

  // 判断是否为 OTA 命令主题（检查 topic 是否包含 /ota/cmd）
  if (strstr(topic, "/ota/cmd") != NULL) {
    Serial.printf("[OTA-CMD] 收到消息 topic=%s len=%d\n", topic, len);
    Serial.printf("[OTA-CMD] Payload: %.*s\n", (int)len, payload);
    // 使用新的 mqtt_ota_handler 处理
    mqtt_ota_handle_command(topic, (const uint8_t*)payload, len);
    return;  // OTA topic is always consumed, including malformed commands.
  }

  // ---- /cmd 下行命令：提取 task_id / t，过期校验，回复 ACK ----
  bool is_cmd_topic = (strcmp(sub, "cmd") == 0);
  if (is_cmd_topic) {
    StaticJsonDocument<1024> doc;
    DeserializationError json_error = deserializeJson(doc, payload, len);
    const char* task_id = doc["task_id"] | "";
    const char* cmd_str = doc["cmd"] | "";
    bool has_numeric_t = doc["t"].is<long>() || doc["t"].is<unsigned long>();
    time_t cmd_t = has_numeric_t ? doc["t"].as<time_t>() : 0;
    time_t now = time(nullptr);

    if (json_error || !task_id[0] || strlen(task_id) >= 64 ||
        !cmd_str[0] || strlen(cmd_str) >= 32 || !has_numeric_t) {
      char invalid_resp[256];
      snprintf(invalid_resp, sizeof(invalid_resp),
        "{\"task_id\":\"%s\",\"cmd\":\"%s\",\"stage\":\"%s\","
        "\"success\":false,\"code\":\"%s\",\"message\":\"task_id, cmd and numeric t are required\",\"result\":[]}",
        task_id, cmd_str, PROTO_CMD_STAGE_DONE, PROTO_ERR_INVALID_ARGS);
      mqtt_publish_envelope(PROTO_TOPIC_CMD_RESP, invalid_resp, strlen(invalid_resp));
      Serial.println("[CMD] Rejected malformed command");
      return;
    }

    RecentTask* duplicate = find_task(task_id);
    if (duplicate) {
      mqtt_publish_envelope(PROTO_TOPIC_CMD_RESP, duplicate->response,
                            strlen(duplicate->response));
      Serial.printf("[CMD] Duplicate response replayed: %s\n", task_id);
      return;
    }

    // 过期校验（NTP 未同步时跳过）
    if (now > 0 && cmd_t > 0 && (now - cmd_t) > PROTO_CMD_EXPIRE_S) {
      char expired_resp[256];
      snprintf(expired_resp, sizeof(expired_resp),
        "{\"task_id\":\"%s\",\"cmd\":\"%s\",\"stage\":\"%s\","
        "\"success\":false,\"code\":\"%s\",\"message\":\"Command expired\",\"result\":[]}",
        task_id, cmd_str, PROTO_CMD_STAGE_DONE, PROTO_ERR_EXPIRED);
      mqtt_publish_envelope(PROTO_TOPIC_CMD_RESP, expired_resp, strlen(expired_resp));
      Serial.printf("[CMD] Expired: task_id=%s cmd=%s t=%ld now=%ld\n",
                    task_id, cmd_str, (long)cmd_t, (long)now);
      return;  // 不转发给 ARM
    }

    char ack_resp[256];
    snprintf(ack_resp, sizeof(ack_resp),
      "{\"task_id\":\"%s\",\"cmd\":\"%s\",\"stage\":\"%s\","
      "\"success\":true,\"code\":\"%s\",\"message\":\"\",\"result\":[]}",
      task_id, cmd_str, PROTO_CMD_STAGE_ACK, PROTO_ERR_OK);
    bool delivered = uart_send_frame(CMD_COMMAND_RECV,
                                     (const uint8_t*)payload, len);
    if (!delivered) {
      Serial.printf("[CMD] UART delivery failed: %s\n", task_id);
      return;
    }
    remember_task(task_id, ack_resp);
    mqtt_publish_envelope(PROTO_TOPIC_CMD_RESP, ack_resp, strlen(ack_resp));
    Serial.printf("[CMD] ACK sent: task_id=%s cmd=%s\n", task_id, cmd_str);
    return;
  }

  // 直接转发原始 MQTT payload 给 ARM（不包装 JSON，避免嵌套引号问题）
  uart_send_frame(CMD_COMMAND_RECV, (const uint8_t*)payload, len);
  Serial.printf("[MQTT→ARM] topic=%s len=%d\n", sub, len);
  Serial.printf("[MQTT→ARM] Payload: %.*s\n", (int)len, payload);
}

// ==================== 按键 ====================
static void check_button() {
  bool pressed = (digitalRead(PIN_BTN) == LOW);
  if (pressed && !g_btn_pressed) {
    g_btn_pressed = true;
    g_btn_press_start = millis();
  }
  if (!pressed && g_btn_pressed) {
    g_btn_pressed = false;
    unsigned long duration = millis() - g_btn_press_start;
    if (duration >= BUTTON_LONG_PRESS) {
      Serial.println("[BTN] Factory reset!");
      led1_set(LED_3FLASH);     // 两个LED同步快闪=恢复出厂确认
      led2_set(LED_3FLASH);
      offline_cache_factory_reset();
      clear_persisted_task_response();
      ota_result_clear();
      storage_factory_reset();
      delay(1500);
      ESP.restart();
    } else if (duration > 200 && duration < BUTTON_LONG_PRESS) {
      Serial.println("[BTN] -> Toggle AP mode");
      if (g_sys_state == STATE_AP_PROVISION) {
        if (storage_has_wifi_config()) {
          storage_get_wifi(g_wifi_ssid, sizeof(g_wifi_ssid), g_wifi_pass, sizeof(g_wifi_pass));
          init_wifi_sta();
          g_sys_state = STATE_STA_CONNECTING;
          led1_set(LED_FAST_BLINK);   // LED1 快闪=WiFi连接中
          led2_set(LED_OFF);
        }
      } else {
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
        char ap_ssid[32];
        build_ap_ssid(ap_ssid, sizeof(ap_ssid));
        WiFi.softAP(ap_ssid, NULL, 1, 0, 1);
        Serial.printf("[AP] softAP(%s) started alongside STA\n", ap_ssid);
        tcp_provision_init();
      }
    }
  }
}
