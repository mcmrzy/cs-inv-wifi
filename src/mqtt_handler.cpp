/**
 * mqtt_handler.cpp — MQTT 客户端 (PubSubClient + TLS)
 * 
 * 使用 PubSubClient 库替代 ESP-MQTT，更好的 Arduino TLS 支持
 */

#include "mqtt_handler.h"
#include "nvs_storage.h"
#include "sn_encoder.h"
#include "offline_cache.h"
#include "cs_protocol_defs.h"
#include "mqtt_ca_cert.h"
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
static uint32_t g_last_cache_flush = 0;  // 上次缓存刷新时间

static MQTTMessageHandler g_msg_handler = nullptr;

// 设备信息
char g_device_sn[SN_MAX_LEN] = {0};  // 与 main.cpp 保持一致
char g_device_key[65] = {0};

// 离线缓存发送回调函数
static bool offline_send_callback(const char* topic, const char* payload, uint32_t timestamp) {
    if (!g_mqtt_client || !g_mqtt_client->connected()) {
        Serial.println("[OFFLINE-SEND] MQTT not connected");
        return false;
    }
    
    // 构建完整主题
    char full_topic[128];
    int n = snprintf(full_topic, sizeof(full_topic), PROTO_TOPIC_FMT, g_device_sn, topic);
    if (n < 0 || n >= (int)sizeof(full_topic)) {
        Serial.println("[OFFLINE-SEND] Topic too long");
        return false;
    }
    
    // 统一信封格式：{"t":NTP秒,"v":1,"data":<payload>}
    char full_payload[1024];
    int len = snprintf(full_payload, sizeof(full_payload),
        "{\"t\":%lu,\"v\":%d,\"data\":%s}", (unsigned long)timestamp, PROTO_ENVELOPE_VERSION, payload);
    
    if (len <= 0 || len >= (int)sizeof(full_payload)) {
        Serial.println("[OFFLINE-SEND] Payload too large");
        return false;
    }
    
    // 直接发布，避免mqtt_publish再次添加时间戳
    bool result = g_mqtt_client->publish(full_topic, (const uint8_t*)full_payload, len, false);
    Serial.printf("[OFFLINE-SEND] topic=%s, ts=%u, result=%d\n", topic, timestamp, result);
    return result;
}

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

    // mqtt_init() is also used after SN/broker changes. Release the previous
    // pair before rebuilding it so repeated reconfiguration does not leak heap.
    if (g_mqtt_client) {
        if (g_mqtt_client->connected()) g_mqtt_client->disconnect();
        delete g_mqtt_client;
        g_mqtt_client = nullptr;
    }
    if (g_wifi_client) {
        g_wifi_client->stop();
        delete g_wifi_client;
        g_wifi_client = nullptr;
    }

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
    char key_buf[65] = {0};
    if (storage_get_key(key_buf, sizeof(key_buf))) {
        strncpy(g_device_key, key_buf, sizeof(g_device_key) - 1);
        Serial.printf("[MQTT] Loaded key length: %zu\n", strlen(g_device_key));
    }

    char mqtt_host[64] = MQTT_DEFAULT_HOST;
    uint16_t mqtt_port = MQTT_DEFAULT_PORT;
    storage_get_mqtt(mqtt_host, sizeof(mqtt_host), &mqtt_port);

    // 构建 MQTT URI
    snprintf(g_mqtt_uri, sizeof(g_mqtt_uri),
             "%s:%d", mqtt_host, mqtt_port);
    Serial.printf("[MQTT] Broker: %s (TLS)\n", g_mqtt_uri);

    // 构建状态主题
    snprintf(g_status_topic, sizeof(g_status_topic),
             PROTO_TOPIC_FMT, g_device_sn, PROTO_TOPIC_STATUS);
    snprintf(g_lwt_payload, sizeof(g_lwt_payload),
             "{\"t\":0,\"v\":%d,\"data\":{\"online\":false}}", PROTO_ENVELOPE_VERSION);
    Serial.printf("[MQTT] Status topic: %s\n", g_status_topic);

    // 创建 WiFiClientSecure
    g_wifi_client = new WiFiClientSecure();
    if (!g_wifi_client) {
        Serial.println("[MQTT] Failed to create WiFiClientSecure");
        return false;
    }

    // Fail closed: authenticate the broker with the provisioned CA.
    g_wifi_client->setCACert(MQTT_CA_CERT);

    // 创建 PubSubClient
    g_mqtt_client = new PubSubClient(*g_wifi_client);
    if (!g_mqtt_client) {
        Serial.println("[MQTT] Failed to create PubSubClient");
        return false;
    }

    g_mqtt_client->setServer(mqtt_host, mqtt_port);
    g_mqtt_client->setCallback(mqtt_callback);
    g_mqtt_client->setBufferSize(2048);

    Serial.printf("[MQTT] Final URI: mqtts://%s\n", g_mqtt_uri);
    return true;
}

// ==================== 连接 ====================
bool mqtt_connect(void) {
    return mqtt_connect_ex(false);
}

bool mqtt_connect_ex(bool force) {
    if (!g_mqtt_client || !WiFi.isConnected()) {
        return false;
    }

    // 已经连接
    if (g_mqtt_client->connected()) {
        return true;
    }

    // 重连间隔控制（强制模式跳过）
    uint32_t now = millis();
    if (!force && (now - g_last_reconnect < g_reconnect_delay)) {
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
        g_device_key[0] ? g_device_key : MQTT_PASSWORD,
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

        // 发布上线状态（信封格式）
        char online_payload[128];
        time_t online_ts = time(nullptr);
        snprintf(online_payload, sizeof(online_payload),
                 "{\"t\":%lu,\"v\":%d,\"data\":{\"online\":true,\"rssi\":%d}}",
                 (unsigned long)online_ts, PROTO_ENVELOPE_VERSION, WiFi.RSSI());
        g_mqtt_client->publish(g_status_topic, online_payload, true);

        // 订阅命令主题
        char cmd_topic[64];
        snprintf(cmd_topic, sizeof(cmd_topic), "cs_inv/%s/cmd", g_device_sn);
        g_mqtt_client->subscribe(cmd_topic);
        Serial.printf("[MQTT] Subscribed to: %s\n", cmd_topic);

        // 订阅 OTA 命令主题
        snprintf(cmd_topic, sizeof(cmd_topic), "cs_inv/%s/ota/cmd", g_device_sn);
        g_mqtt_client->subscribe(cmd_topic);
        Serial.printf("[MQTT] Subscribed to: %s\n", cmd_topic);

        // 发送离线缓存数据
        int cached_count = offline_cache_count();
        if (cached_count > 0) {
            Serial.printf("[MQTT] Found %d cached items, flushing...\n", cached_count);
            offline_cache_flush_all(offline_send_callback);
        }
        g_last_cache_flush = millis();  // 记录刷新时间，避免立即重试

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

// 信封格式发布：{"t":<NTP秒>,"data":<arm_payload>}
bool mqtt_publish_envelope(const char *topic, const char *arm_payload, unsigned int payload_len, bool retain) {
    if (!g_mqtt_client || !g_mqtt_client->connected()) return false;

    char full_topic[128];
    int n = snprintf(full_topic, sizeof(full_topic), PROTO_TOPIC_FMT, g_device_sn, topic);
    if (n < 0 || n >= (int)sizeof(full_topic)) return false;

    time_t now = time(nullptr);
    // 外层信封：{"t":NTP秒,"v":1,"data":<payload>}
    size_t env_len = payload_len + 64;
    char *envelope = (char*)malloc(env_len);
    if (!envelope) return false;
    snprintf(envelope, env_len, "{\"t\":%lu,\"v\":%d,\"data\":%s}", (unsigned long)now, PROTO_ENVELOPE_VERSION, arm_payload);

    bool result = g_mqtt_client->publish(full_topic, (const uint8_t*)envelope, strlen(envelope), retain);
    free(envelope);
    return result;
}

int mqtt_publish(const char* sub_topic, const char* data, int len, int qos, int retain) {
    if (!sub_topic || !data || len < 0) {
        return -2;
    }

    static bool qos_warning_logged = false;
    if (qos > 0 && !qos_warning_logged) {
        qos_warning_logged = true;
        Serial.println("[MQTT] WARNING: PubSubClient outbound publish is QoS 0 only; QoS 1 requires an MQTT client migration");
    }

    // 如果未连接，存储到离线缓存
    if (!g_mqtt_client || !g_mqtt_client->connected()) {
        if (offline_cache_add(sub_topic, data, 0)) {
            Serial.printf("[MQTT] Not connected, cached: topic=%s (%d bytes)\n", sub_topic, len);
            return -1;  // not sent, durably cached
        }
        Serial.printf("[MQTT] Not connected and cache failed: topic=%s\n", sub_topic);
        return -4;
    }

    // 委托给信封格式发布
    bool result = mqtt_publish_envelope(sub_topic, data, (unsigned int)len, retain != 0);
    if (result) return 0;

    if (offline_cache_add(sub_topic, data, 0)) {
        Serial.printf("[MQTT] Publish failed, cached: topic=%s (%d bytes)\n", sub_topic, len);
        return -1;
    }
    return -4;
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
    int n = snprintf(full_topic, sizeof(full_topic), PROTO_TOPIC_FMT, g_device_sn, sub_topic);
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
            g_mqtt_connected = false;
            mqtt_connect();
        }
        g_mqtt_client->loop();
        
        // 定期检查并发送离线缓存数据（每30秒）
        if (g_mqtt_connected && g_mqtt_client->connected()) {
            uint32_t now = millis();
            if (now - g_last_cache_flush >= 30000) {
                g_last_cache_flush = now;
                int cached = offline_cache_count();
                if (cached > 0) {
                    Serial.printf("[MQTT] Found %d cached items, flushing...\n", cached);
                    offline_cache_flush_all(offline_send_callback);
                }
            }
        }
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
