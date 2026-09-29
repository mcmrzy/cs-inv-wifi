/**
 * mqtt_handler.c — MQTT 客户端 (ESP-IDF esp_mqtt + TLS)
 *
 * 移植自 esp32c2_idf (裁剪 OTA 部分), 保留:
 *  - 信封 {"t":..,"v":1,"data":..} + cs_inv/{sn}/ 主题前缀
 *  - 断线重连 (退避) / 在线状态 (online + rssi + ip) / LWT 遗嘱
 *  - 离线缓存补传 (QoS1 + PUBACK 等待)
 */
#include "mqtt_handler.h"
#include "mqtt_broker_route.h"
#include "mqtt_envelope.h"
#include "config.h"
#include "nvs_storage.h"
#include "offline_cache.h"
#include "cs_protocol_defs.h"
#include "wifi_sta.h"
#include "telemetry.h"
#include "cmd_handler.h"
#include "ota_mqtt_adapter.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <time.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_tls_errors.h"
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
static atomic_bool g_broker_fallback_allowed;
static atomic_bool g_use_broker_ip_fallback;
static atomic_bool g_broker_fallback_pending;
static int64_t g_disconnect_start_ms = 0;  /* 断线开始时刻, 用于检测卡住 */
static int g_force_restart_count = 0;      /* 连续强制重启次数, 动态延长超时 */
static int64_t g_last_cache_flush = 0;

static MQTTMessageHandler g_msg_handler = NULL;
static MQTTMessageHandler g_msg_handler_saved = NULL;  /* force_restart 用 */

static atomic_bool g_flush_pending;
static atomic_bool g_cache_flush_running;
static QueueHandle_t g_offline_ack_queue = NULL;

#define OFFLINE_ACK_TIMEOUT_MS  5000
/* 补传任务内包含 offline_send_callback 的 1KB 级局部缓冲、
 * esp_mqtt_client_publish 内部调用链及 ESP_LOGI 格式化，
 * 2048 字节会发生栈溢出 (Stack protection fault)，需留足余量 */
#define OFFLINE_FLUSH_TASK_STACK 4096

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

    /* 大缓冲放堆上，避免占用已紧张的补传任务栈 */
    char *full_topic = malloc(128);
    if (!full_topic) {
        return false;
    }
    int n = snprintf(full_topic, 128, PROTO_TOPIC_FMT,
                     g_device_sn, topic);
    if (n < 0 || n >= 128) {
        free(full_topic);
        return false;
    }

    char *full_payload = malloc(1024);
    if (!full_payload) {
        free(full_topic);
        return false;
    }
    int len = snprintf(full_payload, 1024,
        "{\"t\":%" PRIu32 ",\"v\":%d,\"data\":%s}",
        timestamp, PROTO_ENVELOPE_VERSION, payload);
    if (len <= 0 || len >= 1024) {
        free(full_topic);
        free(full_payload);
        return false;
    }

    int ack_id = -1;
    while (xQueueReceive(g_offline_ack_queue, &ack_id, 0) == pdTRUE) {}

    const int msg_id = esp_mqtt_client_publish(g_mqtt_client, full_topic,
                                               full_payload, len, 1, 0);
    free(full_topic);
    free(full_payload);
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
        atomic_store(&g_broker_fallback_pending, false);
        g_disconnect_start_ms = 0;
        g_force_restart_count = 0;

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
        if (g_disconnect_start_ms == 0) {
            g_disconnect_start_ms = esp_timer_get_time() / 1000;
        }
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
        if (event->error_handle) {
            const esp_mqtt_error_codes_t *err = event->error_handle;
            ESP_LOGE(TAG,
                     "MQTT_EVENT_ERROR type=%d tls_esp=0x%x(%s) "
                     "tls_stack=0x%x cert_flags=0x%x sock_errno=%d(%s) "
                     "connect_rc=%d",
                     (int)err->error_type,
                     (unsigned)err->esp_tls_last_esp_err,
                     esp_err_to_name(err->esp_tls_last_esp_err),
                     (unsigned)err->esp_tls_stack_err,
                     (unsigned)err->esp_tls_cert_verify_flags,
                     err->esp_transport_sock_errno,
                     strerror(err->esp_transport_sock_errno),
                     (int)err->connect_return_code);
            const bool cannot_resolve =
                err->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT &&
                err->esp_tls_last_esp_err ==
                    ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME;
            if (mqtt_broker_route_should_activate(
                    atomic_load(&g_broker_fallback_allowed), cannot_resolve,
                    atomic_load(&g_use_broker_ip_fallback))) {
                atomic_store(&g_use_broker_ip_fallback, true);
                atomic_store(&g_broker_fallback_pending, true);
                ESP_LOGW(TAG,
                         "DNS resolution failed; scheduling default broker "
                         "fallback to %s",
                         MQTT_DEFAULT_FALLBACK_IP);
            }
        } else {
            ESP_LOGE(TAG, "MQTT_EVENT_ERROR (no error handle)");
        }
        break;

    default:
        break;
    }
}

bool mqtt_init(MQTTMessageHandler msg_handler)
{
    g_msg_handler = msg_handler;
    g_msg_handler_saved = msg_handler;
    mqtt_rx_reset();
    g_disconnect_start_ms = 0;

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
    const bool fallback_allowed = strcmp(broker_host, MQTT_DEFAULT_HOST) == 0;
    atomic_store(&g_broker_fallback_allowed, fallback_allowed);
    const char *connect_host = mqtt_broker_route_select(
        broker_host, MQTT_DEFAULT_HOST, MQTT_DEFAULT_FALLBACK_IP,
        atomic_load(&g_use_broker_ip_fallback));
    snprintf(g_mqtt_uri, sizeof(g_mqtt_uri), "mqtts://%s:%u",
             connect_host, broker_port);
    ESP_LOGI(TAG, "Broker: %s", g_mqtt_uri);
    if (connect_host != broker_host) {
        ESP_LOGW(TAG, "Using default broker IP fallback for this boot");
    }

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
        .network.reconnect_timeout_ms = 10000,
        .session.keepalive = MQTT_KEEPALIVE,
        .task.stack_size = 8192,  /* OTA JSON 解析需要较大栈空间 */
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

    ESP_LOGI(TAG, "Starting MQTT client to %s", g_mqtt_uri);

    esp_err_t err = esp_mqtt_client_start(g_mqtt_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_start failed: %s", esp_err_to_name(err));
        return false;
    }

    atomic_store(&g_mqtt_started, true);
    if (g_disconnect_start_ms == 0) {
        g_disconnect_start_ms = esp_timer_get_time() / 1000;
    }
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

/* 发布核心: 拼接主题前缀 + 信封。
 * allow_cache=false → 只实时发, 断连即丢 (调试日志之类补传无价值的上行,
 * 不该挤占离线缓存里遥测的补传槽位)。
 * 注意: 这里不碰遥测计时器 —— 心跳周期只由 telemetry_tick 管理; 曾经"任何上行
 * 都重置心跳计时器"会让 90s 在线上报不断推后 180s 心跳, 正常周期下心跳永不发出。 */
static int publish_core(const char *sub_topic, const char *data, int qos,
                        int retain, bool allow_cache, uint16_t version)
{
    if (!sub_topic || !data) {
        return -2;
    }

    char full_topic[128];
    int n = snprintf(full_topic, sizeof(full_topic), "cs_inv/%s/%s",
                     g_device_sn, sub_topic);
    if (n < 0 || n >= (int)sizeof(full_topic)) {
        return -3;
    }

    if (!g_mqtt_client || !atomic_load(&g_mqtt_connected)) {
        if (!allow_cache) {
            return -1;
        }
        bool stored = offline_cache_add(sub_topic, data, 0);
        ESP_LOGW(TAG, "Not connected, cache %s: topic=%s",
                 stored ? "stored" : "failed", sub_topic);
        return stored ? 1 : -1;
    }

    uint32_t timestamp = (uint32_t)time(NULL);
    char *full_payload = NULL;
    size_t payload_len = 0;
    const int format_result = mqtt_envelope_build(data, timestamp, version,
                                                   &full_payload, &payload_len);
    if (format_result != 0) {
        ESP_LOGE(TAG, "MQTT envelope build failed: code=%d free=%u max=%u",
                 format_result, (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        return format_result;
    }

    int msg_id = esp_mqtt_client_publish(g_mqtt_client, full_topic,
                                         full_payload, (int)payload_len, qos, retain);
    free(full_payload);
    if (msg_id >= 0) {
        return 0;
    }
    if (!allow_cache) {
        return -4;
    }
    return offline_cache_add(sub_topic, data, timestamp) ? 1 : -4;
}

/* 清掉指定 SN 的 retained status (空载荷 + retain=1 = 让 broker 删除该保留消息)。
 * 用途: SN 从 MAC 派生值换成 ARM 真实值后, 旧主题上的 retained 会变成"幽灵设备"
 * 永久挂在设备列表里 (2026-08-15 那批 L10<MAC> 主题就是这么留下的);
 * 换 SN 前先把旧的清掉, 一次到位。 */
void mqtt_clear_retained_status(const char *sn)
{
    if (!sn || !sn[0] || !g_mqtt_client || !atomic_load(&g_mqtt_connected)) {
        return;
    }
    char topic[128];
    int n = snprintf(topic, sizeof(topic), PROTO_TOPIC_FMT, sn, PROTO_TOPIC_STATUS);
    if (n <= 0 || n >= (int)sizeof(topic)) return;
    esp_mqtt_client_publish(g_mqtt_client, topic, "", 0, 1, 1);
    ESP_LOGW(TAG, "cleared retained status of old SN: %s", sn);
}

int mqtt_publish(const char *sub_topic, const char *data, int len, int qos,
                 int retain)
{
    if (len < 0) {
        return -2;
    }
    return publish_core(sub_topic, data, qos, retain, true, PROTO_ENVELOPE_VERSION);
}

int mqtt_publish_volatile(const char *sub_topic, const char *data, int len,
                          int qos)
{
    if (len < 0) {
        return -2;
    }
    return publish_core(sub_topic, data, qos, 0, false, PROTO_ENVELOPE_VERSION);
}

/* heartbeat v3: ARM 运行参数原样转发专用 (信封版本 3, 不走离线缓存)。
 * 不缓存的理由: ① 心跳是周期状态快照, 补传过期快照价值低;
 * ② 离线缓存项 payload 上限 512B, 87 值结构体 payload ≈530B 会被拒;
 * ③ 缓存补传固定按 v2 重组信封, 会把 v3 内容包成 v2 导致服务端按错版本解析。 */
int mqtt_publish_heartbeat_raw(const char *sub_topic, const char *data, int len)
{
    if (len < 0) {
        return -2;
    }
    return publish_core(sub_topic, data, 0, 0, false,
                        PROTO_HEARTBEAT_ENVELOPE_VERSION);
}

void mqtt_publish_ota_trace(const char *stage, const char *detail)
{
    char buf[512];
    size_t n = 0;
    const char *p;

    if (!stage) stage = "";
    if (!detail) detail = "";

    n += (size_t)snprintf(buf + n, sizeof(buf) - n, "{\"stage\":\"");
    for (p = stage; *p && n + 4 < sizeof(buf); ++p) {
        buf[n++] = (*p == '"' || *p == '\\') ? '\'' : *p;
    }
    n += (size_t)snprintf(buf + n, sizeof(buf) - n, "\",\"detail\":\"");
    for (p = detail; *p && n + 8 < sizeof(buf); ++p) {
        const unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            buf[n++] = '\'';            /* JSON 里不能裸传引号/反斜杠 */
        } else if (c < 0x20) {
            buf[n++] = ' ';             /* 控制字符(含换行)替换为空格, 保持单行 */
        } else {
            buf[n++] = (char)c;         /* ASCII 与 UTF-8 原样保留 */
        }
    }
    /* 若在 UTF-8 多字节字符中间被截断, 回退到完整字符边界, 免得 JSON 非法 */
    while (n > 0) {
        const unsigned char c = (unsigned char)buf[n - 1];
        if ((c & 0xC0) == 0x80) { n--; continue; }  /* 续字节: 继续回退 */
        if (c >= 0xC0) { n--; }                     /* 首字节: 整个字符丢弃 */
        break;
    }
    if (n + 3 >= sizeof(buf)) return;

    n += (size_t)snprintf(buf + n, sizeof(buf) - n, "\"}");
    mqtt_publish_volatile(PROTO_TOPIC_OTA_DEBUG, buf, (int)n, 0);
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

bool mqtt_is_stuck(int64_t timeout_ms)
{
    if (atomic_load(&g_mqtt_connected)) return false;
    if (atomic_load(&g_broker_fallback_pending)) return true;
    if (g_disconnect_start_ms == 0) return false;
    /* 连续重启次数越多, 超时越长 (上限 10 分钟) */
    int64_t effective = timeout_ms * (1 + g_force_restart_count);
    if (effective > 600000) effective = 600000;
    int64_t now = esp_timer_get_time() / 1000;
    return (now - g_disconnect_start_ms) >= effective;
}

void mqtt_force_restart(void)
{
    const bool broker_fallback =
        atomic_exchange(&g_broker_fallback_pending, false);
    g_force_restart_count++;
    ESP_LOGW(TAG, "Force restarting MQTT client (count=%d, broker_fallback=%d)",
             g_force_restart_count, broker_fallback);
    mqtt_disconnect();
    if (!mqtt_init(g_msg_handler_saved)) {
        ESP_LOGE(TAG, "MQTT client reinitialization failed");
    }
}
