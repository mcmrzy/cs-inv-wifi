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
#include "sn_encoder.h"
#include "ip_geolocation.h"
#include <esp_mac.h>

// min 宏定义
#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif

#ifdef CHIP_ESP32C3
  #include <WiFi.h>
  #include "esp_task_wdt.h"
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

static unsigned long g_last_heartbeat = 0;
static unsigned long g_last_led_tick = 0;
static unsigned long g_last_debug = 0;

static bool g_btn_pressed = false;
static unsigned long g_btn_press_start = 0;

// ==================== 前向声明 ====================
static void init_wifi_sta();
static void state_ap_provision();
static void state_sta_connecting();
static void state_mqtt_connecting();
static void state_running();
static void on_uart_frame(uint8_t cmd, const uint8_t* data, uint16_t len);
static void on_provision_done(const char* ssid, const char* password);
static void on_mqtt_message(const char* topic, const char* payload, unsigned int len);
static void check_button();
static void build_ap_ssid(char* out, size_t out_len);

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  delay(3000);
  Serial.println("\n\n========================================");
  Serial.println(" INVERTER WiFi MODULE (Arduino)");
  Serial.println(" ESP32-C3 + Custom UART + MQTT");
  Serial.println("========================================");

  storage_init();
  uart_protocol_init();
  led_init();

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
  ipgeo_init();  // 初始化 IP 定位模块

  if (storage_has_wifi_config()) {
    storage_get_wifi(g_wifi_ssid, sizeof(g_wifi_ssid),
                     g_wifi_pass, sizeof(g_wifi_pass));
    Serial.printf("[SYS] Saved WiFi: %s, pass_len=%d\n", g_wifi_ssid, strlen(g_wifi_pass));
    init_wifi_sta();
    g_sys_state = STATE_STA_CONNECTING;
    led1_set(LED_FAST_BLINK);   // LED1 快闪=WiFi连接中
    led2_set(LED_OFF);
    tcp_provision_init();
  } else {
    Serial.println("[SYS] No WiFi -> AP Provision mode");
    char ap_ssid[32];
    build_ap_ssid(ap_ssid, sizeof(ap_ssid));
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(200);
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
    WiFi.softAP(ap_ssid, NULL, 1, 0, 1);
    Serial.printf("[AP] softAP(%s) IP=%s\n", ap_ssid, WiFi.softAPIP().toString().c_str());
    g_sys_state = STATE_AP_PROVISION;
    led1_set(LED_SLOW_BLINK);   // LED1 慢闪=AP配网模式
    led2_set(LED_OFF);
    tcp_provision_init();
  }

  configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org");
  g_last_debug = millis();
  Serial.println("[SYS] Setup done");
}

// ==================== LOOP ====================
void loop() {
  unsigned long now = millis();

  // 1. UART 数据处理（最优先）
  uart_protocol_process();

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
      break;
    case STATE_OTA:
      yield();
      break;
  }

  // 5. 定期调试输出（每 60 秒）
  if (now - g_last_debug >= 60000) {
    g_last_debug = now;
    Serial.printf("[DBG] State=%d, WiFi=%d, Heap=%d\n",
                  g_sys_state, WiFi.status(), ESP.getFreeHeap());
  }

  // 6. 喂狗
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
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
  WiFi.softAP(ap_ssid, NULL, 1, 0, 1);
  WiFi.begin(g_wifi_ssid, g_wifi_pass);
  g_wifi_connect_start = millis();
  g_wifi_reconnect_attempts = 0;
  Serial.printf("[WIFI] Connecting to %s...\n", g_wifi_ssid);
}

static void state_sta_connecting() {
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WIFI] Connected! STA_IP=%s AP_IP=%s\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.softAPIP().toString().c_str());
    g_wifi_reconnect_attempts = 0;
    g_wifi_reconnect_delay = 1000;
    
    // WiFi 连接成功后触发 IP 定位
    ipgeo_request_update();
    
    g_sys_state = STATE_MQTT_CONNECTING;
    led1_set(LED_SOLID);       // LED1 常亮=WiFi已连
    led2_set(LED_SLOW_BLINK);  // LED2 慢闪=MQTT连接中
    // LWT 在 mqtt_connect() 中自动设置
    return;
  }
  if (millis() - g_wifi_connect_start > 15000) {
    g_wifi_reconnect_attempts++;
    Serial.printf("[WIFI] Retry %d\n", g_wifi_reconnect_attempts);
    if (g_wifi_reconnect_attempts >= 10) {
      Serial.println("[WIFI] Max retries -> AP mode");
      storage_clear_wifi();
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      delay(200);
      WiFi.mode(WIFI_AP);
      WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
      char ap_ssid[32];
      build_ap_ssid(ap_ssid, sizeof(ap_ssid));
      WiFi.softAP(ap_ssid, NULL, 1, 0, 1);
      g_sys_state = STATE_AP_PROVISION;
      led1_set(LED_SLOW_BLINK);   // LED1 慢闪=AP配网
      led2_set(LED_OFF);
      tcp_provision_init();
      return;
    }
    delay(g_wifi_reconnect_delay);
    WiFi.disconnect(false);
    WiFi.begin(g_wifi_ssid, g_wifi_pass);
    g_wifi_connect_start = millis();
    g_wifi_reconnect_delay = min(g_wifi_reconnect_delay * 2, 60000UL);
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
    g_last_heartbeat = millis();
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
  
  // 处理 IP 定位（非阻塞检查）
  ipgeo_check_result(nullptr);

  // 心跳
  if (millis() - g_last_heartbeat >= HEARTBEAT_INTERVAL) {
    g_last_heartbeat = millis();
    
    // 构建带地理位置的 status 消息
    char location_json[256];
    GeoLocation geo;
    if (ipgeo_get_cached(&geo)) {
      ipgeo_to_json(&geo, location_json, sizeof(location_json));
    } else {
      strncpy(location_json, "null", sizeof(location_json));
    }
    
    char status_msg[512];
    snprintf(status_msg, sizeof(status_msg),
      "{\"online\":true,\"rssi\":%d,\"location\":%s}",
      WiFi.RSSI(), location_json);
    
    mqtt_publish("status", status_msg, strlen(status_msg), 1, 1);
    Serial.printf("[HB] status sent, location=%s\n", ipgeo_status_string());
  }
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

      // 辅助：从 JSON 提取字符串
      auto extract_str = [](char* js, const char* key, char* out, int max_len) -> bool {
        char* p = strstr(js, key);
        if (p) {
          p = strchr(p + strlen(key), ':');
          if (p) {
            p = strchr(p + 1, '"');
            if (p) {
              p++;
              int i = 0;
              while (*p && *p != '"' && i < max_len - 1) {
                if (*p == '\\' && *(p + 1)) p++;
                out[i++] = *p++;
              }
              out[i] = '\0';
              return true;
            }
          }
        }
        return false;
      };

      char topic[64] = {0};
      char payload[FRAME_DATA_MAX - 80] = {0};
      char sn[SN_MAX_LEN] = {0};

      extract_str(json, "\"topic\"", topic, sizeof(topic));
      extract_str(json, "\"payload\"", payload, sizeof(payload));
      if (extract_str(json, "\"sn\"", sn, sizeof(sn))) {
        if (strlen(sn) > 0 && strcmp(sn, g_device_sn) != 0) {
          // ---- SN 格式校验 (CS-SN-STD-001 V1.1) ----
          if (!sn_validate(sn)) {
            Serial.printf("[UART] PUBLISH SN REJECTED (invalid): %s\n", sn);
            uart_send_nack();
            return;  // 不继续发布, break 会跳出 switch 但不执行 uart_send_ack
          }
          storage_set_sn(sn);
          strncpy(g_device_sn, sn, sizeof(g_device_sn) - 1);
          Serial.printf("[UART] SN updated via PUBLISH: %s\n", sn);
          mqtt_disconnect();
          mqtt_init(on_mqtt_message);
          if (g_sys_state == STATE_RUNNING) g_sys_state = STATE_MQTT_CONNECTING;
          char ap_ssid[32];
          build_ap_ssid(ap_ssid, sizeof(ap_ssid));
          WiFi.softAP(ap_ssid, NULL, 1, 0, 1);
        }
      }

      if (strlen(topic) > 0) {
        mqtt_publish(topic, payload, strlen(payload), 0, 0);
        led_signal_data();    // LED2 短暂闪一下表示有数据活动
        Serial.printf("[UART] PUBLISH -> cs_inv/%s/%s (%d bytes)\n",
                      g_device_sn, topic, strlen(payload));
      } else {
        Serial.println("[UART] PUBLISH error: topic empty");
      }
      uart_send_ack();
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
      storage_factory_reset();
      uart_send_ack();
      delay(100);
      ESP.restart();
      break;

    default:
      uart_send_nack();
      break;
  }
}

// ==================== 配网回调 ====================
static void on_provision_done(const char* ssid, const char* password) {
  Serial.printf("[PROV] Got: %s, pass_len=%d\n", ssid, strlen(password));
  storage_set_wifi(ssid, password);
  strncpy(g_wifi_ssid, ssid, sizeof(g_wifi_ssid) - 1);
  strncpy(g_wifi_pass, password, sizeof(g_wifi_pass) - 1);
  init_wifi_sta();
  g_sys_state = STATE_STA_CONNECTING;
  led1_set(LED_FAST_BLINK);   // LED1 快闪=配网成功，WiFi连接中
  led2_set(LED_OFF);
  Serial.printf("[PROV] AP kept running, connecting STA to %s\n", ssid);
}

// ==================== MQTT 消息回调 (云端 → ESP32 → ARM) ====================
static void on_mqtt_message(const char* topic, const char* payload, unsigned int len) {
  const char* sub = strrchr(topic, '/');
  if (sub) sub++; else sub = topic;

  // 构造 JSON 转发给 ARM
  char json[512];
  int jslen = snprintf(json, sizeof(json),
    "{\"topic\":\"%s\",\"payload\":\"%.*s\"}", sub, (int)len, payload);
  uart_send_frame(CMD_COMMAND_RECV, (const uint8_t*)json, jslen);
  Serial.printf("[MQTT→ARM] topic=%s len=%d\n", sub, len);
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
