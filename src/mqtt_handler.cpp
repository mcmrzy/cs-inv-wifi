/**
 * mqtt_handler.cpp — MQTT 客户端 (PubSubClient + TLS)
 * 
 * 使用 PubSubClient 库替代 ESP-MQTT，更好的 Arduino TLS 支持
 */

#include "mqtt_handler.h"
#include "nvs_storage.h"
#include "sn_encoder.h"
#include <string.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

// ==================== 全局状态 ====================
static WiFiClientSecure* g_wifi_client = nullptr;
static PubSubClient* g_mqtt_client = nullptr;

static char g_mqtt_uri[128] = {0};
static char g_status_topic[64] = {0};
static char g_lwt_payload[128] = {0};

static bool g_mqtt_connected = false;
static uint32_t g_last_reconnect = 0;
static uint32_t g_reconnect_delay = MQTT_RECONNECT_MIN;
static int g_reconnect_attempts = 0;

static MQTTMessageHandler g_msg_handler = nullptr;

// 设备信息
char g_device_sn[SN_MAX_LEN] = {0};  // 与 main.cpp 保持一致
char g_device_key[65] = {0};

// ==================== 回调函数 ====================
static void mqtt_callback(char* topic, byte* payload, unsigned int length) {
    if (!g_msg_handler) return;
    
    // 复制 payload（PubSubClient 的 payload 不是 null-terminated）
    char* buf = (char*)malloc(length + 1);
    if (!buf) return;
    memcpy(buf, payload, length);
    buf[length] = '\0';
    
    g_msg_handler(topic, buf, length);
    free(buf);
}

// ==================== 初始化 ====================
bool mqtt_init(MQTTMessageHandler msg_handler) {
    g_msg_handler = msg_handler;

    // 从 NVS 读取 SN
    char sn_buf[SN_FULL_LEN + 1] = {0};
    if (!storage_get_sn(sn_buf, sizeof(sn_buf))) {
        Serial.println("[MQTT] SN not set in NVS");
        return false;
    }
    if (!sn_validate(sn_buf)) {
        Serial.printf("[MQTT] Invalid SN in NVS: %s\n", sn_buf);
        return false;
    }
    strncpy(g_device_sn, sn_buf, sizeof(g_device_sn) - 1);
    Serial.printf("[MQTT] Loaded SN: %s (valid)\n", g_device_sn);

    // 读取 key（可选）
    char key_buf[33] = {0};
    if (storage_get_key(key_buf, sizeof(key_buf))) {
        strncpy(g_device_key, key_buf, sizeof(g_device_key) - 1);
        Serial.printf("[MQTT] Loaded key length: %zu\n", strlen(g_device_key));
    }

    // 构建 MQTT URI
    snprintf(g_mqtt_uri, sizeof(g_mqtt_uri),
             "%s:%d", MQTT_DEFAULT_HOST, MQTT_DEFAULT_PORT);
    Serial.printf("[MQTT] Broker: %s (TLS)\n", g_mqtt_uri);

    // 构建状态主题
    snprintf(g_status_topic, sizeof(g_status_topic),
             "cs_inv/%s/status", g_device_sn);
    snprintf(g_lwt_payload, sizeof(g_lwt_payload),
             "{\"online\":false,\"sn\":\"%s\"}", g_device_sn);
    Serial.printf("[MQTT] Status topic: %s\n", g_status_topic);

    // 创建 WiFiClientSecure
    g_wifi_client = new WiFiClientSecure();
    if (!g_wifi_client) {
        Serial.println("[MQTT] Failed to create WiFiClientSecure");
        return false;
    }

    // 配置 TLS：跳过证书验证（私有 Broker，生产环境建议配置正确的 CA 证书）
    g_wifi_client->setInsecure();

    // 创建 PubSubClient
    g_mqtt_client = new PubSubClient(*g_wifi_client);
    if (!g_mqtt_client) {
        Serial.println("[MQTT] Failed to create PubSubClient");
        return false;
    }

    g_mqtt_client->setServer(MQTT_DEFAULT_HOST, MQTT_DEFAULT_PORT);
    g_mqtt_client->setCallback(mqtt_callback);
    g_mqtt_client->setBufferSize(1024);  // 增大缓冲区

    Serial.printf("[MQTT] Final URI: mqtts://%s\n", g_mqtt_uri);
    return true;
}

// ==================== 连接 ====================
bool mqtt_connect(void) {
    if (!g_mqtt_client || !WiFi.isConnected()) {
        return false;
    }

    // 已经连接
    if (g_mqtt_client->connected()) {
        return true;
    }

    // 重连间隔控制
    uint32_t now = millis();
    if (now - g_last_reconnect < g_reconnect_delay) {
        return false;
    }
    g_last_reconnect = now;

    g_reconnect_attempts++;
    Serial.printf("[MQTT] Reconnect attempt %d\n", g_reconnect_attempts);

    Serial.printf("[MQTT] Connecting to %s (TLS)...\n", g_mqtt_uri);

    // 连接参数
    bool connected = g_mqtt_client->connect(
        g_device_sn,           // Client ID
        MQTT_USERNAME,         // Username
        MQTT_PASSWORD,         // Password
        g_status_topic,        // Will Topic
        1,                     // Will QoS
        true,                  // Will Retain
        g_lwt_payload          // Will Message
    );

    if (connected) {
        Serial.println("[MQTT] Connected!");
        g_mqtt_connected = true;
        g_reconnect_attempts = 0;
        g_reconnect_delay = MQTT_RECONNECT_MIN;

        // 发布上线状态
        char online_payload[128];
        snprintf(online_payload, sizeof(online_payload),
                 "{\"online\":true,\"sn\":\"%s\"}", g_device_sn);
        g_mqtt_client->publish(g_status_topic, online_payload, true);

        // 订阅命令主题
        char cmd_topic[64];
        snprintf(cmd_topic, sizeof(cmd_topic), "cs_inv/%s/cmd", g_device_sn);
        g_mqtt_client->subscribe(cmd_topic);
        Serial.printf("[MQTT] Subscribed to: %s\n", cmd_topic);

        return true;
    } else {
        int state = g_mqtt_client->state();
        Serial.printf("[MQTT] Connection failed, state=%d\n", state);
        
        // 指数退避
        g_reconnect_delay = min(g_reconnect_delay * 2, (uint32_t)MQTT_RECONNECT_MAX);
        
        // 最大重试次数检查
        if (g_reconnect_attempts >= MQTT_RECONNECT_MAX_ATTEMPTS) {
            Serial.println("[MQTT] Max retries reached");
        }
        
        return false;
    }
}

// ==================== 断开连接 ====================
void mqtt_disconnect(void) {
    if (g_mqtt_client && g_mqtt_client->connected()) {
        g_mqtt_client->disconnect();
    }
    g_mqtt_connected = false;
}

// ==================== 发布 ====================
int mqtt_publish(const char* sub_topic, const char* data, int len, int qos, int retain) {
    if (!g_mqtt_client || !g_mqtt_client->connected()) {
        return -1;
    }

    if (!sub_topic || !data || len < 0) {
        return -2;
    }

    // 构建完整主题
    char full_topic[128];
    int n = snprintf(full_topic, sizeof(full_topic), "cs_inv/%s/%s", g_device_sn, sub_topic);
    if (n < 0 || n >= (int)sizeof(full_topic)) {
        return -3;
    }

    bool result = g_mqtt_client->publish(full_topic, (const uint8_t*)data, len, retain);
    return result ? 0 : -4;
}

// ==================== 订阅 ====================
int mqtt_subscribe(const char* sub_topic, int qos) {
    if (!g_mqtt_client || !g_mqtt_client->connected()) {
        return -1;
    }

    if (!sub_topic) {
        return -2;
    }

    // 构建完整主题
    char full_topic[128];
    int n = snprintf(full_topic, sizeof(full_topic), "cs_inv/%s/%s", g_device_sn, sub_topic);
    if (n < 0 || n >= (int)sizeof(full_topic)) {
        return -3;
    }

    bool result = g_mqtt_client->subscribe(full_topic, qos);
    return result ? 0 : -4;
}

// ==================== 轮询 ====================
void mqtt_loop(void) {
    if (g_mqtt_client) {
        // 保持连接
        if (!g_mqtt_client->connected()) {
            mqtt_connect();
        }
        g_mqtt_client->loop();
    }
}

// ==================== 状态查询 ====================
bool mqtt_is_connected(void) {
    return g_mqtt_client && g_mqtt_client->connected();
}

const char* mqtt_get_client_id(void) {
    return g_device_sn;
}

// ==================== 设置 Broker ====================
bool mqtt_set_broker(const char* host, uint16_t port) {
    if (!host || strlen(host) < 1 || strlen(host) > 63) {
        return false;
    }
    if (port == 0) {
        return false;
    }

    // 更新配置
    snprintf(g_mqtt_uri, sizeof(g_mqtt_uri), "%s:%d", host, port);
    
    if (g_mqtt_client) {
        g_mqtt_client->setServer(host, port);
    }
    
    // 保存到 NVS
    storage_set_mqtt(host, port);
    
    Serial.printf("[MQTT] Broker set to: %s\n", g_mqtt_uri);
    return true;
}

// ==================== 获取主题 ====================
const char* mqtt_get_status_topic(void) {
    return g_status_topic;
}

const char* mqtt_get_lwt_payload(void) {
    return g_lwt_payload;
}
