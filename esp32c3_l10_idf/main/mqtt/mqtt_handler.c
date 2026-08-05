/**
 * mqtt_handler.c — MQTT 客户端 (ESP-IDF esp_mqtt + TLS)
 *
 * 移植自 esp32c2_idf (裁剪 OTA 部分), 保留:
 *  - 信封 {"t":..,"v":1,"data":..} + cs_inv/{sn}/ 主题前缀
 *  - 断线重连 (退避) / 在线状态 (online + rssi + ip) / LWT 遗嘱
 *  - 离线缓存补传 (QoS1 + PUBACK 等待)
 */
#include "mqtt_handler.h"
#include "nvs_storage.h"
#include "offline_cache.h"
#include "cs_protocol_defs.h"
#include "wifi_sta.h"
#include "telemetry.h"
#include "cmd_handler.h"
#include "ota_mqtt_adapter.h"
#include "cJSON.h"
#include <string.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "MQTT";

#define MQTT_URI_MAX     128
#define MQTT_TOPIC_MAX   64
#define MQTT_PAYLOAD_MAX 128
#define MQTT_RX_MAX      2048

static esp_mqtt_client_handle_t g_mqtt_client = NULL;

static char g_mqtt_uri[MQTT_URI_MAX] = {0};
static char g_status_topic[MQTT_TOPIC_MAX] = {0};
static char g_lwt_payload[MQTT_PAYLOAD_MAX] = {0};

static atomic_bool g_mqtt_connected;
static atomic_bool g_mqtt_started;
static int64_t g_last_reconnect = 0;
static int64_t g_reconnect_delay = MQTT_RECONNECT_MIN;
static int g_reconnect_attempts = 0;
static int64_t g_last_cache_flush = 0;

static MQTTMessageHandler g_msg_handler = NULL;

static atomic_bool g_flush_pending;
static atomic_bool g_cache_flush_running;
static QueueHandle_t g_offline_ack_queue = NULL;

#define OFFLINE_ACK_TIMEOUT_MS  5000
#define OFFLINE_FLUSH_TASK_STACK 2048

static char g_rx_topic[128];
static char g_rx_payload[MQTT_RX_MAX + 1];
static int g_rx_expected;
static int g_rx_next_offset;

char g_device_sn[SN_MAX_LEN] = {0};
char g_device_key[65] = {0};

static void mqtt_rx_reset(void)
{
    g_rx_expected = 0;
    g_rx_next_offset = 0;
    g_rx_topic[0] = '\0';
}

static bool offline_send_callback(const char *topic, const char *payload,
                                  uint32_t timestamp)
{
    if (!g_mqtt_client || !g_offline_ack_queue ||
        !atomic_load(&g_mqtt_connected)) {
        return false;
    }

    char full_topic[128];
    int n = snprintf(full_topic, sizeof(full_topic), PROTO_TOPIC_FMT,
                     g_device_sn, topic);
    if (n < 0 || n >= (int)sizeof(full_topic)) {
        return false;
    }

    char full_payload[1024];
    int len = snprintf(full_payload, sizeof(full_payload),
        "{\"t\":%" PRIu32 ",\"v\":%d,\"data\":%s}",
        timestamp, PROTO_ENVELOPE_VERSION, payload);
    if (len <= 0 || len >= (int)sizeof(full_payload)) {
        return false;
    }

    int ack_id = -1;
    while (xQueueReceive(g_offline_ack_queue, &ack_id, 0) == pdTRUE) {}

    const int msg_id = esp_mqtt_client_publish(g_mqtt_client, full_topic,
                                               full_payload, len, 1, 0);
    if (msg_id < 0) return false;

    const TickType_t started = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(OFFLINE_ACK_TIMEOUT_MS);
    bool acknowledged = false;
    while (atomic_load(&g_mqtt_connected)) {
        const TickType_t elapsed = xTaskGetTickCount() - started;
        if (elapsed >= timeout) break;
        if (xQueueReceive(g_offline_ack_queue, &ack_id,
                          timeout - elapsed) != pdTRUE) break;
        if (ack_id == msg_id) {
            acknowledged = true;
            break;
        }
    }
    acknowledged = acknowledged && atomic_load(&g_mqtt_connected);

    ESP_LOGI(TAG, "[OFFLINE-SEND] topic=%s, ts=%u, msg_id=%d, ack=%d",
             topic, timestamp, msg_id, acknowledged);
    return acknowledged;
}

static void offline_cache_flush_task(void *parameter)
{
    (void)parameter;
    offline_cache_flush_all(offline_send_callback);
    atomic_store(&g_cache_flush_running, false);
    vTaskDelete(NULL);
}

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    (void)event_id;
    esp_mqtt_event_handle_t event = event_data;

    switch (event->event_id) {
    case MQTT_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        atomic_store(&g_mqtt_connected, true);
        atomic_store(&g_mqtt_started, true);
        g_reconnect_attempts = 0;
        g_reconnect_delay = MQTT_RECONNECT_MIN;

        /* status: online + rssi + ip */
        {
            char ip[32] = "0.0.0.0";
            wifi_sta_get_ip(ip, sizeof(ip));
            char online_payload[192];
            snprintf(online_payload, sizeof(online_payload),
                "{\"t\":%ld,\"v\":%d,\"data\":{\"online\":true,"
                "\"rssi\":%d,\"ip\":\"%s\"}}",
                (long)time(NULL), PROTO_ENVELOPE_VERSION,
                wifi_sta_get_rssi(), ip);
            esp_mqtt_client_publish(g_mqtt_client, g_status_topic,
                                    online_payload, 0, 1, 1);
        }

        /* 订阅云端命令 */
        {
            char cmd_topic[64];
            snprintf(cmd_topic, sizeof(cmd_topic), "cs_inv/%s/cmd", g_device_sn);
            esp_mqtt_client_subscribe(g_mqtt_client, cmd_topic, 1);
            ESP_LOGI(TAG, "Subscribed to: %s", cmd_topic);
        }

        /* 订阅云端 OTA 命令 */
        {
            char ota_topic[64];
            snprintf(ota_topic, sizeof(ota_topic), "cs_inv/%s/ota/cmd", g_device_sn);
            esp_mqtt_client_subscribe(g_mqtt_client, ota_topic, 1);
            ESP_LOGI(TAG, "Subscribed to: %s", ota_topic);
        }

        /* 触发遥测立即上报 + 离线补传 */
        telemetry_on_mqtt_connected();
        {
            int cached_count = offline_cache_count();
            if (cached_count > 0) {
                ESP_LOGI(TAG, "Found %d cached items, will flush", cached_count);
                atomic_store(&g_flush_pending, true);
            }
            g_last_cache_flush = esp_timer_get_time() / 1000;
        }
        break;
    }

    case MQTT_EVENT_DATA:
        if (!g_msg_handler || event->data_len <= 0) break;
        if (event->total_data_len <= 0 || event->total_data_len > MQTT_RX_MAX ||
            event->current_data_offset < 0 ||
            event->current_data_offset + event->data_len > event->total_data_len) {
            ESP_LOGE(TAG, "Invalid/oversized MQTT fragment");
            mqtt_rx_reset();
            break;
        }
        if (event->current_data_offset == 0) {
            if (event->topic_len <= 0 || event->topic_len >= (int)sizeof(g_rx_topic)) {
                mqtt_rx_reset();
                break;
            }
            memcpy(g_rx_topic, event->topic, event->topic_len);
            g_rx_topic[event->topic_len] = '\0';
            g_rx_expected = event->total_data_len;
            g_rx_next_offset = 0;
        }
        if (g_rx_expected != event->total_data_len ||
            event->current_data_offset != g_rx_next_offset) {
            ESP_LOGE(TAG, "Out-of-sequence MQTT fragment");
            mqtt_rx_reset();
            break;
        }
        memcpy(g_rx_payload + event->current_data_offset, event->data,
               event->data_len);
        g_rx_next_offset += event->data_len;
        if (event->current_data_offset + event->data_len == g_rx_expected) {
            g_rx_payload[g_rx_expected] = '\0';
            /* OTA 命令主题分流, 其余交给 cmd_handler */
            if (strstr(g_rx_topic, "/ota/cmd") != NULL) {
                ESP_LOGI(TAG, "OTA cmd topic=%s len=%u", g_rx_topic, g_rx_expected);
                mqtt_ota_handle_command(g_rx_topic,
                                        (const uint8_t *)g_rx_payload,
                                        (unsigned int)g_rx_expected);
            } else if (g_msg_handler) {
                g_msg_handler(g_rx_topic, g_rx_payload,
                              (unsigned int)g_rx_expected);
            }
            mqtt_rx_reset();
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT_EVENT_DISCONNECTED");
        atomic_store(&g_mqtt_connected, false);
        if (g_offline_ack_queue) {
            const int disconnected = -1;
            xQueueSend(g_offline_ack_queue, &disconnected, 0);
        }
        mqtt_rx_reset();
        break;

    case MQTT_EVENT_PUBLISHED:
        if (g_offline_ack_queue) {
            const int published = event->msg_id;
            if (xQueueSend(g_offline_ack_queue, &published, 0) != pdTRUE) {
                int discarded;
                xQueueReceive(g_offline_ack_queue, &discarded, 0);
                xQueueSend(g_offline_ack_queue, &published, 0);
            }
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT_EVENT_ERROR");
        break;

    default:
        break;
    }
}

bool mqtt_init(MQTTMessageHandler msg_handler)
{
    g_msg_handler = msg_handler;
    mqtt_rx_reset();
    g_last_reconnect = 0;
    g_reconnect_delay = MQTT_RECONNECT_MIN;
    g_reconnect_attempts = 0;

    if (!g_offline_ack_queue) {
        g_offline_ack_queue = xQueueCreate(8, sizeof(int));
        if (!g_offline_ack_queue) {
            ESP_LOGE(TAG, "Failed to create offline PUBACK queue");
            return false;
        }
    }

    if (g_mqtt_client) {
        esp_mqtt_client_stop(g_mqtt_client);
        esp_mqtt_client_destroy(g_mqtt_client);
        g_mqtt_client = NULL;
        atomic_store(&g_mqtt_connected, false);
        atomic_store(&g_mqtt_started, false);
    }

    if (!g_device_sn[0]) {
        ESP_LOGE(TAG, "SN not set");
        return false;
    }
    ESP_LOGI(TAG, "Loaded SN: %s", g_device_sn);

    if (!g_device_key[0]) {
        ESP_LOGE(TAG, "MQTT device key is not provisioned");
        return false;
    }

    char broker_host[64] = MQTT_DEFAULT_HOST;
    uint16_t broker_port = MQTT_DEFAULT_PORT;
    storage_get_mqtt(broker_host, sizeof(broker_host), &broker_port);
    snprintf(g_mqtt_uri, sizeof(g_mqtt_uri), "mqtts://%s:%u",
             broker_host, broker_port);
    ESP_LOGI(TAG, "Broker: %s", g_mqtt_uri);

    snprintf(g_status_topic, sizeof(g_status_topic), PROTO_TOPIC_FMT,
             g_device_sn, PROTO_TOPIC_STATUS);
    snprintf(g_lwt_payload, sizeof(g_lwt_payload),
             "{\"t\":0,\"v\":%d,\"data\":{\"online\":false}}",
             PROTO_ENVELOPE_VERSION);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = g_mqtt_uri,
        .credentials.username = MQTT_USERNAME,
        .credentials.authentication.password = MQTT_PASSWORD,
        .credentials.client_id = g_device_sn,
        .session.last_will = {
            .topic = g_status_topic,
            .msg = g_lwt_payload,
            .qos = 1,
            .retain = 1,
        },
        /* 自有固定服务器: 不配置 CA, 配合 CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY 跳过证书验证 (TLS 加密仍在; OTA 固件有 Ed25519 签名兜底) */
        .broker.verification.skip_cert_common_name_check = true,
        .network.reconnect_timeout_ms = MQTT_RECONNECT_MIN,
        .task.stack_size = 4096,
    };

    g_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!g_mqtt_client) {
        ESP_LOGE(TAG, "Failed to create esp_mqtt_client");
        return false;
    }

    esp_mqtt_client_register_event(g_mqtt_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);

    ESP_LOGI(TAG, "MQTT client initialized: %s", g_mqtt_uri);
    return true;
}

bool mqtt_connect(void)
{
    if (!g_mqtt_client) return false;
    if (atomic_load(&g_mqtt_connected)) return true;
    if (atomic_load(&g_mqtt_started)) return true;

    int64_t now = esp_timer_get_time() / 1000;
    if (now - g_last_reconnect < g_reconnect_delay) {
        return false;
    }
    g_last_reconnect = now;

    g_reconnect_attempts++;
    ESP_LOGI(TAG, "Connect attempt %d to %s", g_reconnect_attempts, g_mqtt_uri);

    esp_err_t err = esp_mqtt_client_start(g_mqtt_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_start failed: %s", esp_err_to_name(err));
        g_reconnect_delay = MIN(g_reconnect_delay * 2, MQTT_RECONNECT_MAX);
        if (g_reconnect_attempts >= MQTT_RECONNECT_MAX_ATTEMPTS) {
            ESP_LOGW(TAG, "Max retries reached");
        }
        return false;
    }

    atomic_store(&g_mqtt_started, true);
    return true;
}

void mqtt_disconnect(void)
{
    mqtt_rx_reset();
    if (g_mqtt_client) {
        esp_mqtt_client_stop(g_mqtt_client);
        esp_mqtt_client_destroy(g_mqtt_client);
        g_mqtt_client = NULL;
    }
    atomic_store(&g_mqtt_connected, false);
    atomic_store(&g_mqtt_started, false);
}

int mqtt_publish(const char *sub_topic, const char *data, int len, int qos,
                 int retain)
{
    if (!sub_topic || !data || len < 0) {
        return -2;
    }

    char full_topic[128];
    int n = snprintf(full_topic, sizeof(full_topic), PROTO_TOPIC_FMT,
                     g_device_sn, sub_topic);
    if (n < 0 || n >= (int)sizeof(full_topic)) {
        return -3;
    }

    if (!g_mqtt_client || !atomic_load(&g_mqtt_connected)) {
        bool stored = offline_cache_add(sub_topic, data, 0);
        ESP_LOGW(TAG, "Not connected, cache %s: topic=%s",
                 stored ? "stored" : "failed", sub_topic);
        return stored ? 1 : -1;
    }

    char full_payload[1024];
    uint32_t timestamp = (uint32_t)time(NULL);
    int payload_len = snprintf(full_payload, sizeof(full_payload),
        "{\"t\":%" PRIu32 ",\"v\":%d,\"data\":%s}",
        timestamp, PROTO_ENVELOPE_VERSION, data);
    if (payload_len <= 0 || payload_len >= (int)sizeof(full_payload)) {
        ESP_LOGE(TAG, "Payload too large");
        return -5;
    }

    int msg_id = esp_mqtt_client_publish(g_mqtt_client, full_topic,
                                         full_payload, payload_len, qos, retain);
    if (msg_id >= 0) return 0;
    return offline_cache_add(sub_topic, data, timestamp) ? 1 : -4;
}

int mqtt_subscribe(const char *sub_topic, int qos)
{
    if (!g_mqtt_client || !atomic_load(&g_mqtt_connected)) {
        return -1;
    }
    if (!sub_topic) {
        return -2;
    }

    char full_topic[128];
    int n = snprintf(full_topic, sizeof(full_topic), PROTO_TOPIC_FMT,
                     g_device_sn, sub_topic);
    if (n < 0 || n >= (int)sizeof(full_topic)) {
        return -3;
    }

    int msg_id = esp_mqtt_client_subscribe(g_mqtt_client, full_topic, qos);
    return (msg_id >= 0) ? 0 : -4;
}

bool mqtt_is_connected(void)
{
    return atomic_load(&g_mqtt_connected);
}

const char *mqtt_get_client_id(void)
{
    return g_device_sn;
}

void mqtt_flush_offline_cache(void)
{
    if (!g_mqtt_client || !atomic_load(&g_mqtt_connected)) {
        return;
    }

    int cached = offline_cache_count();
    if (cached == 0) {
        atomic_store(&g_flush_pending, false);
        return;
    }

    if (atomic_exchange(&g_flush_pending, false)) {
        ESP_LOGI(TAG, "Flushing %d cached items (priority)...", cached);
    }

    bool expected = false;
    if (!atomic_compare_exchange_strong(&g_cache_flush_running,
                                        &expected, true)) {
        return;
    }
    if (xTaskCreate(offline_cache_flush_task, "cache_flush",
                    OFFLINE_FLUSH_TASK_STACK, NULL, 4, NULL) != pdPASS) {
        atomic_store(&g_cache_flush_running, false);
        ESP_LOGE(TAG, "Failed to create offline cache flush task");
    }
}
