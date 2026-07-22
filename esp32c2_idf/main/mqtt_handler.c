/**
 * mqtt_handler.c — MQTT 客户端 (ESP-IDF esp_mqtt + TLS)
 *
 * 使用 esp_mqtt_client 替代 PubSubClient，事件驱动模型
 */

#include "mqtt_handler.h"
#include "nvs_storage.h"
#include "offline_cache.h"
#include "mqtt_ca_cert.h"
#include "cs_protocol_defs.h"
#include "ota_service.h"
#include "cJSON.h"
#include <math.h>
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

static const char* TAG = "MQTT";

#define MQTT_URI_MAX    128
#define MQTT_TOPIC_MAX  64
#define MQTT_PAYLOAD_MAX 128

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

#define OFFLINE_ACK_TIMEOUT_MS 5000
#define OFFLINE_FLUSH_TASK_STACK 2048

#define MQTT_RX_MAX 4096
static char g_rx_topic[128];
static char g_rx_payload[MQTT_RX_MAX + 1];
static int g_rx_expected;
static int g_rx_next_offset;

static void mqtt_rx_reset(void) {
    g_rx_expected = 0;
    g_rx_next_offset = 0;
    g_rx_topic[0] = '\0';
}

char g_device_sn[SN_MAX_LEN] = {0};
char g_device_key[65] = {0};

static bool offline_send_callback(const char* topic, const char* payload, uint32_t timestamp) {
    if (!g_mqtt_client || !g_offline_ack_queue || !atomic_load(&g_mqtt_connected)) {
        ESP_LOGW(TAG, "[OFFLINE-SEND] MQTT not connected");
        return false;
    }

    char full_topic[128];
    int n = snprintf(full_topic, sizeof(full_topic), PROTO_TOPIC_FMT, g_device_sn, topic);
    if (n < 0 || n >= (int)sizeof(full_topic)) {
        ESP_LOGW(TAG, "[OFFLINE-SEND] Topic too long");
        return false;
    }

    char full_payload[1024];
    int len = snprintf(full_payload, sizeof(full_payload),
        "{\"t\":%" PRIu32 ",\"v\":%d,\"data\":%s}", timestamp, PROTO_ENVELOPE_VERSION, payload);

    if (len <= 0 || len >= (int)sizeof(full_payload)) {
        ESP_LOGW(TAG, "[OFFLINE-SEND] Payload too large");
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
        if (xQueueReceive(g_offline_ack_queue, &ack_id, timeout - elapsed) != pdTRUE) break;
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

static void offline_cache_flush_task(void* parameter) {
    (void)parameter;
    offline_cache_flush_all(offline_send_callback);
    atomic_store(&g_cache_flush_running, false);
    vTaskDelete(NULL);
}

static void mqtt_event_handler(void* arg, esp_event_base_t base,
                                int32_t event_id, void* event_data) {
    (void)arg;
    (void)base;
    (void)event_id;
    esp_mqtt_event_handle_t event = event_data;

    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            atomic_store(&g_mqtt_connected, true);
            atomic_store(&g_mqtt_started, true);
            g_reconnect_attempts = 0;
            g_reconnect_delay = MQTT_RECONNECT_MIN;

            {
                char online_payload[128];
                snprintf(online_payload, sizeof(online_payload),
                         "{\"t\":%ld,\"v\":%d,\"data\":{\"online\":true}}",
                         (long)time(NULL), PROTO_ENVELOPE_VERSION);
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
                    atomic_store(&g_flush_pending, true);
                }
                g_last_cache_flush = esp_timer_get_time() / 1000;
            }
            break;

        case MQTT_EVENT_DATA:
            if (!g_msg_handler || event->data_len <= 0) break;
            if (event->total_data_len <= 0 || event->total_data_len > MQTT_RX_MAX ||
                event->current_data_offset < 0 ||
                event->current_data_offset + event->data_len > event->total_data_len) {
                ESP_LOGE(TAG, "Invalid/oversized MQTT fragment: %d/%d at %d",
                         event->data_len, event->total_data_len,
                         event->current_data_offset);
                mqtt_rx_reset();
                break;
            }
            if (event->current_data_offset == 0) {
                if (event->topic_len <= 0 || event->topic_len >= (int)sizeof(g_rx_topic)) {
                    ESP_LOGE(TAG, "MQTT topic too long");
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
            memcpy(g_rx_payload + event->current_data_offset, event->data, event->data_len);
            g_rx_next_offset += event->data_len;
            if (event->current_data_offset + event->data_len == g_rx_expected) {
                g_rx_payload[g_rx_expected] = '\0';
                g_msg_handler(g_rx_topic, g_rx_payload, (unsigned int)g_rx_expected);
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

bool mqtt_init(MQTTMessageHandler msg_handler) {
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

    if (!storage_get_sn(g_device_sn, sizeof(g_device_sn))) {
        ESP_LOGE(TAG, "SN not set in NVS");
        return false;
    }
    ESP_LOGI(TAG, "Loaded SN: %s", g_device_sn);

    char key_buf[sizeof(g_device_key)] = {0};
    if (storage_get_key(key_buf, sizeof(key_buf))) {
        strncpy(g_device_key, key_buf, sizeof(g_device_key) - 1);
    }
    if (!g_device_key[0]) {
        ESP_LOGE(TAG, "MQTT device key is not provisioned");
        return false;
    }

    char broker_host[64] = MQTT_DEFAULT_HOST;
    uint16_t broker_port = MQTT_DEFAULT_PORT;
    storage_get_mqtt(broker_host, sizeof(broker_host), &broker_port);
    snprintf(g_mqtt_uri, sizeof(g_mqtt_uri),
             "mqtts://%s:%u", broker_host, broker_port);
    ESP_LOGI(TAG, "Broker: %s", g_mqtt_uri);

    snprintf(g_status_topic, sizeof(g_status_topic),
             PROTO_TOPIC_FMT, g_device_sn, PROTO_TOPIC_STATUS);
    snprintf(g_lwt_payload, sizeof(g_lwt_payload),
             "{\"t\":0,\"v\":%d,\"data\":{\"online\":false}}", PROTO_ENVELOPE_VERSION);
    ESP_LOGI(TAG, "Status topic: %s", g_status_topic);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = g_mqtt_uri,
        .credentials.username = MQTT_USERNAME,
        .credentials.authentication.password = g_device_key,
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

    if (atomic_load(&g_mqtt_connected)) {
        return true;
    }
    if (atomic_load(&g_mqtt_started)) {
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

    atomic_store(&g_mqtt_started, true);
    return true;
}

void mqtt_on_network_lost(void) {
    mqtt_rx_reset();
    atomic_store(&g_mqtt_connected, false);
    if (g_mqtt_client && atomic_exchange(&g_mqtt_started, false)) {
        esp_err_t err = esp_mqtt_client_stop(g_mqtt_client);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "MQTT pause failed: %s", esp_err_to_name(err));
        }
    }
}

bool mqtt_reconfigure(void) {
    MQTTMessageHandler handler = g_msg_handler;
    mqtt_disconnect();
    return mqtt_init(handler);
}

void mqtt_disconnect(void) {
    mqtt_rx_reset();
    if (g_mqtt_client) {
        esp_mqtt_client_stop(g_mqtt_client);
        esp_mqtt_client_destroy(g_mqtt_client);
        g_mqtt_client = NULL;
    }
    atomic_store(&g_mqtt_connected, false);
    atomic_store(&g_mqtt_started, false);
}

int mqtt_publish(const char* sub_topic, const char* data, int len, int qos, int retain) {
    if (!sub_topic || !data || len < 0) {
        return -2;
    }

    char full_topic[128];
    int n = snprintf(full_topic, sizeof(full_topic), PROTO_TOPIC_FMT, g_device_sn, sub_topic);
    if (n < 0 || n >= (int)sizeof(full_topic)) {
        return -3;
    }

    if (!g_mqtt_client || !atomic_load(&g_mqtt_connected)) {
        bool stored = offline_cache_add(sub_topic, data, 0);
        ESP_LOGW(TAG, "Not connected, cache %s: topic=%s", stored ? "stored" : "failed", sub_topic);
        return stored ? 1 : -1;
    }

    char full_payload[1024];
    uint32_t timestamp = (uint32_t)time(NULL);
    int payload_len = snprintf(full_payload, sizeof(full_payload),
        "{\"t\":%" PRIu32 ",\"v\":%d,\"data\":%s}", timestamp, PROTO_ENVELOPE_VERSION, data);

    if (payload_len <= 0 || payload_len >= (int)sizeof(full_payload)) {
        ESP_LOGE(TAG, "Payload too large");
        return -5;
    }

    int msg_id = esp_mqtt_client_publish(g_mqtt_client, full_topic,
                                          full_payload, payload_len, qos, retain);
    if (msg_id >= 0) return 0;
    return offline_cache_add(sub_topic, data, timestamp) ? 1 : -4;
}

int mqtt_subscribe(const char* sub_topic, int qos) {
    if (!g_mqtt_client || !atomic_load(&g_mqtt_connected)) {
        return -1;
    }

    if (!sub_topic) {
        return -2;
    }

    char full_topic[128];
    int n = snprintf(full_topic, sizeof(full_topic), PROTO_TOPIC_FMT, g_device_sn, sub_topic);
    if (n < 0 || n >= (int)sizeof(full_topic)) {
        return -3;
    }

    int msg_id = esp_mqtt_client_subscribe(g_mqtt_client, full_topic, qos);
    return (msg_id >= 0) ? 0 : -4;
}

bool mqtt_is_connected(void) {
    return atomic_load(&g_mqtt_connected);
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

    if (!storage_set_mqtt(host, port)) return false;

    ESP_LOGI(TAG, "Broker set to: %s", g_mqtt_uri);
    if (!mqtt_reconfigure()) {
        ESP_LOGW(TAG, "Broker persisted; MQTT will initialize after credentials are ready");
    }
    return true;
}

const char* mqtt_get_status_topic(void) {
    return g_status_topic;
}

const char* mqtt_get_lwt_payload(void) {
    return g_lwt_payload;
}

void mqtt_flush_offline_cache(void) {
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
    if (!atomic_compare_exchange_strong(&g_cache_flush_running, &expected, true)) {
        return;
    }
    if (xTaskCreate(offline_cache_flush_task, "cache_flush",
                    OFFLINE_FLUSH_TASK_STACK, NULL, 4, NULL) != pdPASS) {
        atomic_store(&g_cache_flush_running, false);
        ESP_LOGE(TAG, "Failed to create offline cache flush task");
    }
}

/* ================================================================
 *  MQTT OTA command handler (merged from mqtt_ota_adapter.c)
 * ================================================================ */

static const char* OTA_TAG = "mqtt_ota";

/* Request correlation exists only while the MQTT event task handles a command.
 * OTA worker reports use the immutable snapshot stored by ota_service. */
static char g_request_task_id[64];
static char g_request_version[32];
static char g_request_target[8];
static OtaState g_last_published_state = OTA_STATE_IDLE;
static uint8_t g_last_published_progress;

static bool copy_json_string(cJSON* root, const char* name,
                             char* destination, size_t capacity)
{
    cJSON* value = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!value) {
        destination[0] = '\0';
        return true;
    }
    if (!cJSON_IsString(value) || !value->valuestring ||
        strlen(value->valuestring) >= capacity) {
        return false;
    }
    strlcpy(destination, value->valuestring, capacity);
    return true;
}

static bool copy_json_u32(cJSON* root, const char* name, uint32_t* output)
{
    cJSON* value = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!value) return true;
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) ||
        value->valuedouble < 0 || value->valuedouble > UINT32_MAX ||
        floor(value->valuedouble) != value->valuedouble) {
        return false;
    }
    *output = (uint32_t)value->valuedouble;
    return true;
}

static bool parse_ota_command(const uint8_t* payload, unsigned int length,
                              struct MqttOtaCommand* command)
{
    if (!payload || !command || length == 0 || length > 2048) return false;
    cJSON* root = cJSON_ParseWithLength((const char*)payload, length);
    if (!root) return false;

    memset(command, 0, sizeof(*command));
    bool valid = false;
    cJSON* version = cJSON_GetObjectItemCaseSensitive(root, "v");
    if (!cJSON_IsNumber(version) || version->valueint != 1 ||
        version->valuedouble != 1.0) {
        goto done;
    }
    command->protocol_version = 1;

    if (!copy_json_string(root, "action", command->action, sizeof(command->action)) ||
        !copy_json_string(root, "target", command->target, sizeof(command->target)) ||
        !copy_json_string(root, "firmware_id", command->firmware_id, sizeof(command->firmware_id)) ||
        !copy_json_string(root, "url", command->url, sizeof(command->url)) ||
        !copy_json_string(root, "version", command->version, sizeof(command->version)) ||
        !copy_json_string(root, "sha256", command->sha256, sizeof(command->sha256)) ||
        !copy_json_string(root, "signature", command->signature, sizeof(command->signature)) ||
        !copy_json_string(root, "task_id", command->task_id, sizeof(command->task_id)) ||
        !copy_json_string(root, "ota_task_id", command->ota_task_id, sizeof(command->ota_task_id))) {
        goto done;
    }

    cJSON* size = cJSON_GetObjectItemCaseSensitive(root, "size");
    if (!size) size = cJSON_GetObjectItemCaseSensitive(root, "file_size");
    if (size) {
        if (!cJSON_IsNumber(size) || !isfinite(size->valuedouble) ||
            size->valuedouble <= 0 || size->valuedouble > UINT32_MAX ||
            floor(size->valuedouble) != size->valuedouble) {
            goto done;
        }
        command->file_size = (uint32_t)size->valuedouble;
    }
    if (!copy_json_u32(root, "security_version", &command->security_version)) goto done;

    uint32_t timeout = 300;
    if (!copy_json_u32(root, "timeout", &timeout) || timeout > UINT16_MAX) goto done;
    command->timeout = (uint16_t)timeout;

    cJSON* timestamp = cJSON_GetObjectItemCaseSensitive(root, "t");
    if (!timestamp) timestamp = cJSON_GetObjectItemCaseSensitive(root, "timestamp");
    if (!timestamp || !cJSON_IsNumber(timestamp) || !isfinite(timestamp->valuedouble) ||
        timestamp->valuedouble <= 0 || timestamp->valuedouble > UINT32_MAX ||
        floor(timestamp->valuedouble) != timestamp->valuedouble) {
        goto done;
    }
    command->timestamp = (uint32_t)timestamp->valuedouble;
    valid = command->action[0] && command->task_id[0];

done:
    cJSON_Delete(root);
    return valid;
}

static void mqtt_publish_object(cJSON* root, const char* topic)
{
    if (!root) return;
    char* payload = cJSON_PrintUnformatted(root);
    if (payload) {
        mqtt_publish(topic, payload, (int)strlen(payload), 1, 0);
        cJSON_free(payload);
    }
    cJSON_Delete(root);
}

static void publish_snapshot(const OtaSnapshot* snapshot, const char* request_task_id)
{
    if (!snapshot) return;
    cJSON* root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "task_id", snapshot->task_id);
    if (request_task_id && request_task_id[0] &&
        strcmp(request_task_id, snapshot->task_id) != 0) {
        cJSON_AddStringToObject(root, "request_task_id", request_task_id);
    }
    cJSON_AddStringToObject(root, "target", ota_target_name(snapshot->target));
    cJSON_AddStringToObject(root, "version", snapshot->version);
    cJSON_AddStringToObject(root, "source", ota_source_name(snapshot->source));
    cJSON_AddStringToObject(root, "state", ota_state_name(snapshot->state));
    cJSON_AddNumberToObject(root, "progress", snapshot->progress);
    cJSON_AddNumberToObject(root, "received", snapshot->received);
    cJSON_AddNumberToObject(root, "total", snapshot->total);
    cJSON_AddStringToObject(root, "code", snapshot->error_code);
    cJSON_AddStringToObject(root, "message", snapshot->message);
    mqtt_publish_object(root, PROTO_TOPIC_OTA_STATUS);
}

static void ota_status_observer(const OtaSnapshot* snapshot)
{
    if (!snapshot || snapshot->source != OTA_SOURCE_MQTT) return;

    const bool state_changed = snapshot->state != g_last_published_state;
    const bool progress_due = snapshot->progress >= g_last_published_progress + 5;
    if (!state_changed && !progress_due && snapshot->progress != 100) return;

    g_last_published_state = snapshot->state;
    g_last_published_progress = snapshot->progress;
    publish_snapshot(snapshot, NULL);
}

void mqtt_ota_init(void)
{
    g_request_task_id[0] = '\0';
    g_request_version[0] = '\0';
    g_request_target[0] = '\0';
    g_last_published_state = OTA_STATE_IDLE;
    g_last_published_progress = 0;
    ota_service_set_observer(ota_status_observer);
    ESP_LOGI(OTA_TAG, "MQTT OTA command adapter initialized");
}

void mqtt_ota_report_status(const char* target, const char* state, int progress,
                            const char* code, const char* detail)
{
    cJSON* root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "task_id", g_request_task_id);
    cJSON_AddStringToObject(root, "target", target ? target : g_request_target);
    cJSON_AddStringToObject(root, "version", g_request_version);
    cJSON_AddStringToObject(root, "state", state ? state : PROTO_OTA_STATE_ACCEPTED);
    cJSON_AddNumberToObject(root, "progress", progress);
    cJSON_AddStringToObject(root, "code", code ? code : PROTO_ERR_OK);
    cJSON_AddStringToObject(root, "message", detail ? detail : "");
    mqtt_publish_object(root, PROTO_TOPIC_OTA_STATUS);
}

void mqtt_ota_report_error(const char* target, const char* error_code,
                           const char* error_message)
{
    cJSON* root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "task_id", g_request_task_id);
    cJSON_AddStringToObject(root, "target", target ? target : g_request_target);
    cJSON_AddStringToObject(root, "version", g_request_version);
    cJSON_AddStringToObject(root, "state", PROTO_OTA_STATE_FAILED);
    cJSON_AddNumberToObject(root, "progress", 0);
    cJSON_AddStringToObject(root, "code", error_code ? error_code : PROTO_ERR_EXEC_FAILED);
    cJSON_AddStringToObject(root, "message", error_message ? error_message : "OTA failed");
    mqtt_publish_object(root, PROTO_TOPIC_OTA_STATUS);
}

static void send_ota_ack(const char* task_id, bool accepted, const char* message)
{
    cJSON* root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "task_id", task_id ? task_id : "");
    cJSON_AddBoolToObject(root, "ack", accepted);
    cJSON_AddStringToObject(root, "message", message ? message : "");
    mqtt_publish_object(root, PROTO_TOPIC_CMD_RESP);
}

static bool command_expired(const struct MqttOtaCommand* command)
{
    const time_t now = time(NULL);
    return now >= 1704067200 &&
        ((uint32_t)now < command->timestamp ||
         (uint32_t)now - command->timestamp > PROTO_CMD_EXPIRE_S);
}

static void report_service_result(const struct MqttOtaCommand* command, OtaResult result)
{
    if (result == OTA_RESULT_DUPLICATE) {
        send_ota_ack(command->task_id, true, "Duplicate OTA task already active");
        OtaSnapshot snapshot;
        if (ota_service_get_snapshot(&snapshot)) publish_snapshot(&snapshot, command->task_id);
        return;
    }
    send_ota_ack(command->task_id, false, ota_result_code(result));
    mqtt_ota_report_error(command->target, ota_result_code(result), ota_result_code(result));
}

static void handle_start(const struct MqttOtaCommand* command)
{
    if (!command->target[0] || !command->url[0] || !command->version[0] ||
        !command->sha256[0] || !command->signature[0] ||
        command->file_size == 0 || command->security_version == 0) {
        report_service_result(command, OTA_RESULT_INVALID_PARAMS);
        return;
    }
    if (command_expired(command)) {
        send_ota_ack(command->task_id, false, PROTO_ERR_EXPIRED);
        mqtt_ota_report_error(command->target, PROTO_ERR_EXPIRED, "OTA command expired");
        return;
    }

    OtaTarget target;
    if (strcmp(command->target, "esp") == 0) target = OTA_TARGET_ESP;
    else if (strcmp(command->target, "arm") == 0) target = OTA_TARGET_ARM;
    else {
        report_service_result(command, OTA_RESULT_INVALID_TARGET);
        return;
    }

    OtaManifest manifest = {
        .source = OTA_SOURCE_MQTT,
        .target = target,
        .security_version = command->security_version,
        .file_size = command->file_size,
        .timeout_seconds = command->timeout,
        .timestamp = command->timestamp,
    };
    strlcpy(manifest.task_id, command->task_id, sizeof(manifest.task_id));
    strlcpy(manifest.version, command->version, sizeof(manifest.version));
    strlcpy(manifest.url, command->url, sizeof(manifest.url));
    strlcpy(manifest.sha256, command->sha256, sizeof(manifest.sha256));
    strlcpy(manifest.signature, command->signature, sizeof(manifest.signature));

    const OtaResult result = ota_service_start_remote(&manifest);
    if (result == OTA_RESULT_OK) {
        send_ota_ack(command->task_id, true, "OTA task accepted");
    } else {
        report_service_result(command, result);
    }
}

static void handle_cancel(const struct MqttOtaCommand* command)
{
    const char* active_task = command->ota_task_id[0]
        ? command->ota_task_id : ota_service_get_task_id();
    const OtaResult result = ota_service_cancel(active_task);
    if (result == OTA_RESULT_OK) {
        send_ota_ack(command->task_id, true, "OTA cancellation requested");
    } else {
        report_service_result(command, result);
    }
}

static void handle_query(const struct MqttOtaCommand* command)
{
    OtaSnapshot snapshot;
    if (!ota_service_get_snapshot(&snapshot)) {
        report_service_result(command, OTA_RESULT_INTERNAL);
        return;
    }
    send_ota_ack(command->task_id, true, "OTA status follows");
    publish_snapshot(&snapshot, command->task_id);
}

bool mqtt_ota_handle_command(const char* topic, const uint8_t* payload, unsigned int length)
{
    (void)topic;
    struct MqttOtaCommand command;
    if (!parse_ota_command(payload, length, &command)) {
        g_request_task_id[0] = '\0';
        g_request_version[0] = '\0';
        g_request_target[0] = '\0';
        mqtt_ota_report_error("unknown", "JSON_ERROR", "Invalid OTA command");
        return false;
    }

    strlcpy(g_request_task_id, command.task_id, sizeof(g_request_task_id));
    strlcpy(g_request_version, command.version, sizeof(g_request_version));
    strlcpy(g_request_target, command.target, sizeof(g_request_target));

    if (strcmp(command.action, PROTO_OTA_ACTION_START) == 0) {
        handle_start(&command);
    } else if (strcmp(command.action, PROTO_OTA_ACTION_CANCEL) == 0) {
        handle_cancel(&command);
    } else if (strcmp(command.action, PROTO_OTA_ACTION_QUERY) == 0) {
        handle_query(&command);
    } else {
        send_ota_ack(command.task_id, false, PROTO_ERR_NOT_SUPPORTED);
        mqtt_ota_report_error(command.target, PROTO_ERR_NOT_SUPPORTED, "Unsupported OTA action");
    }

    g_request_task_id[0] = '\0';
    g_request_version[0] = '\0';
    g_request_target[0] = '\0';
    return true;
}

void mqtt_ota_process(void)
{
    ota_service_process();
}

bool mqtt_ota_is_busy(void)
{
    return ota_service_is_busy();
}

const char* mqtt_ota_get_task_id(void)
{
    return ota_service_get_task_id();
}
