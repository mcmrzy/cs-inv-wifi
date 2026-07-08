/**
 * mqtt_handler.c — MQTT 客户端 (ESP-IDF esp_mqtt + TLS)
 *
 * 使用 esp_mqtt_client 替代 PubSubClient，事件驱动模型
 */

#include "mqtt_handler.h"
#include "nvs_storage.h"
#include "sn_encoder.h"
#include "offline_cache.h"
#include "mqtt_ca_cert.h"
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_client.h"

static const char* TAG = "MQTT";

#define MQTT_URI_MAX    128
#define MQTT_TOPIC_MAX  64
#define MQTT_PAYLOAD_MAX 128

static esp_mqtt_client_handle_t g_mqtt_client = NULL;

static char g_mqtt_uri[MQTT_URI_MAX] = {0};
static char g_status_topic[MQTT_TOPIC_MAX] = {0};
static char g_lwt_payload[MQTT_PAYLOAD_MAX] = {0};

static bool g_mqtt_connected = false;
static int64_t g_last_reconnect = 0;
static int64_t g_reconnect_delay = MQTT_RECONNECT_MIN;
static int g_reconnect_attempts = 0;
static int64_t g_last_cache_flush = 0;

static MQTTMessageHandler g_msg_handler = NULL;

static bool g_flush_pending = false;

char g_device_sn[SN_MAX_LEN] = {0};
char g_device_key[65] = {0};

static bool offline_send_callback(const char* topic, const char* payload, uint32_t timestamp) {
    if (!g_mqtt_client || !g_mqtt_connected) {
        ESP_LOGW(TAG, "[OFFLINE-SEND] MQTT not connected");
        return false;
    }

    char full_topic[128];
    int n = snprintf(full_topic, sizeof(full_topic), "cs_inv/%s/%s", g_device_sn, topic);
    if (n < 0 || n >= (int)sizeof(full_topic)) {
        ESP_LOGW(TAG, "[OFFLINE-SEND] Topic too long");
        return false;
    }

    char full_payload[1024];
    int len = snprintf(full_payload, sizeof(full_payload),
        "{\"data\":%s,\"timestamp\":%" PRIu32 "}", payload, timestamp);

    if (len <= 0 || len >= (int)sizeof(full_payload)) {
        ESP_LOGW(TAG, "[OFFLINE-SEND] Payload too large");
        return false;
    }

    int msg_id = esp_mqtt_client_publish(g_mqtt_client, full_topic,
                                          full_payload, len, 0, 0);
    ESP_LOGI(TAG, "[OFFLINE-SEND] topic=%s, ts=%u, msg_id=%d", topic, timestamp, msg_id);
    return (msg_id >= 0);
}

static void mqtt_event_handler(void* arg, esp_event_base_t base,
                                int32_t event_id, void* event_data) {
    esp_mqtt_event_handle_t event = event_data;

    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            g_mqtt_connected = true;
            g_reconnect_attempts = 0;
            g_reconnect_delay = MQTT_RECONNECT_MIN;

            {
                char online_payload[128];
                snprintf(online_payload, sizeof(online_payload),
                         "{\"online\":true,\"sn\":\"%s\"}", g_device_sn);
                esp_mqtt_client_publish(g_mqtt_client, g_status_topic,
                                        online_payload, 0, 1, 1);
            }

            {
                char cmd_topic[64];
                snprintf(cmd_topic, sizeof(cmd_topic), "cs_inv/%s/cmd", g_device_sn);
                esp_mqtt_client_subscribe(g_mqtt_client, cmd_topic, 1);
                ESP_LOGI(TAG, "Subscribed to: %s", cmd_topic);

                snprintf(cmd_topic, sizeof(cmd_topic), "cs_inv/%s/ota/cmd", g_device_sn);
                esp_mqtt_client_subscribe(g_mqtt_client, cmd_topic, 1);
                ESP_LOGI(TAG, "Subscribed to: %s", cmd_topic);
            }

            {
                int cached_count = offline_cache_count();
                if (cached_count > 0) {
                    ESP_LOGI(TAG, "Found %d cached items, will flush", cached_count);
                    g_flush_pending = true;
                }
                g_last_cache_flush = esp_timer_get_time() / 1000;
            }
            break;

        case MQTT_EVENT_DATA:
            if (g_msg_handler && event->data_len > 0) {
                char* topic = malloc(event->topic_len + 1);
                char* data = malloc(event->data_len + 1);
                if (topic && data) {
                    memcpy(topic, event->topic, event->topic_len);
                    topic[event->topic_len] = '\0';
                    memcpy(data, event->data, event->data_len);
                    data[event->data_len] = '\0';
                    g_msg_handler(topic, data, (unsigned int)event->data_len);
                }
                free(topic);
                free(data);
            }
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT_EVENT_DISCONNECTED");
            g_mqtt_connected = false;
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT_EVENT_ERROR");
            break;

        default:
            break;
    }
}

bool mqtt_init(MQTTMessageHandler msg_handler) {
    g_msg_handler = msg_handler;

    char sn_buf[SN_BUF_LEN + 1] = {0};
    if (!storage_get_sn(sn_buf, sizeof(sn_buf))) {
        ESP_LOGE(TAG, "SN not set in NVS");
        return false;
    }
    if (!sn_validate(sn_buf)) {
        ESP_LOGE(TAG, "Invalid SN in NVS: %s", sn_buf);
        return false;
    }
    strncpy(g_device_sn, sn_buf, sizeof(g_device_sn) - 1);
    ESP_LOGI(TAG, "Loaded SN: %s (valid)", g_device_sn);

    char key_buf[33] = {0};
    if (storage_get_key(key_buf, sizeof(key_buf))) {
        strncpy(g_device_key, key_buf, sizeof(g_device_key) - 1);
        ESP_LOGI(TAG, "Loaded key length: %zu", strlen(g_device_key));
    }

    snprintf(g_mqtt_uri, sizeof(g_mqtt_uri),
             "mqtts://%s:%d", MQTT_DEFAULT_HOST, MQTT_DEFAULT_PORT);
    ESP_LOGI(TAG, "Broker: %s", g_mqtt_uri);

    snprintf(g_status_topic, sizeof(g_status_topic),
             "cs_inv/%s/status", g_device_sn);
    snprintf(g_lwt_payload, sizeof(g_lwt_payload),
             "{\"online\":false,\"sn\":\"%s\"}", g_device_sn);
    ESP_LOGI(TAG, "Status topic: %s", g_status_topic);

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
        .broker.verification.certificate = MQTT_CA_CERT,
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

    ESP_LOGI(TAG, "MQTT client initialized: mqtts://%s", g_mqtt_uri);
    return true;
}

bool mqtt_connect(void) {
    if (!g_mqtt_client) {
        return false;
    }

    if (g_mqtt_connected) {
        return true;
    }

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

    return true;
}

void mqtt_disconnect(void) {
    if (g_mqtt_client) {
        esp_mqtt_client_stop(g_mqtt_client);
    }
    g_mqtt_connected = false;
}

int mqtt_publish(const char* sub_topic, const char* data, int len, int qos, int retain) {
    if (!sub_topic || !data || len < 0) {
        return -2;
    }

    char full_topic[128];
    int n = snprintf(full_topic, sizeof(full_topic), "cs_inv/%s/%s", g_device_sn, sub_topic);
    if (n < 0 || n >= (int)sizeof(full_topic)) {
        return -3;
    }

    if (!g_mqtt_client || !g_mqtt_connected) {
        offline_cache_add(sub_topic, data, 0);
        ESP_LOGW(TAG, "Not connected, cached: topic=%s (%d bytes)", sub_topic, len);
        return -1;
    }

    char full_payload[1024];
    uint32_t timestamp = (uint32_t)time(NULL);
    int payload_len = snprintf(full_payload, sizeof(full_payload),
        "{\"data\":%s,\"timestamp\":%" PRIu32 "}", data, timestamp);

    if (payload_len <= 0 || payload_len >= (int)sizeof(full_payload)) {
        ESP_LOGE(TAG, "Payload too large");
        return -5;
    }

    int msg_id = esp_mqtt_client_publish(g_mqtt_client, full_topic,
                                          full_payload, payload_len, qos, retain);
    return (msg_id >= 0) ? 0 : -4;
}

int mqtt_subscribe(const char* sub_topic, int qos) {
    if (!g_mqtt_client || !g_mqtt_connected) {
        return -1;
    }

    if (!sub_topic) {
        return -2;
    }

    char full_topic[128];
    int n = snprintf(full_topic, sizeof(full_topic), "cs_inv/%s/%s", g_device_sn, sub_topic);
    if (n < 0 || n >= (int)sizeof(full_topic)) {
        return -3;
    }

    int msg_id = esp_mqtt_client_subscribe(g_mqtt_client, full_topic, qos);
    return (msg_id >= 0) ? 0 : -4;
}

bool mqtt_is_connected(void) {
    return g_mqtt_connected;
}

const char* mqtt_get_client_id(void) {
    return g_device_sn;
}

bool mqtt_set_broker(const char* host, uint16_t port) {
    if (!host || strlen(host) < 1 || strlen(host) > 63) {
        return false;
    }
    if (port == 0) {
        return false;
    }

    snprintf(g_mqtt_uri, sizeof(g_mqtt_uri), "mqtts://%s:%d", host, port);

    storage_set_mqtt(host, port);

    ESP_LOGI(TAG, "Broker set to: %s (restart required)", g_mqtt_uri);
    return true;
}

const char* mqtt_get_status_topic(void) {
    return g_status_topic;
}

const char* mqtt_get_lwt_payload(void) {
    return g_lwt_payload;
}

void mqtt_flush_offline_cache(void) {
    if (!g_mqtt_client || !g_mqtt_connected) {
        return;
    }

    int cached = offline_cache_count();
    if (cached == 0) {
        g_flush_pending = false;
        return;
    }

    if (g_flush_pending) {
        ESP_LOGI(TAG, "Flushing %d cached items (priority)...", cached);
        g_flush_pending = false;
    }

    offline_cache_flush_all(offline_send_callback);
}
