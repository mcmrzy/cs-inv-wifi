#include "state_machine.h"
#include "config.h"
#include "nvs_storage.h"
#include "uart_protocol.h"
#include "led_status.h"
#include "ap_api_server.h"
#include "mqtt_handler.h"
#include "mqtt_ota_handler.h"
#include "ota_service.h"
#include "arm_ota.h"
#include "offline_cache.h"
#include "cs_protocol_defs.h"


#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <math.h>

static const char *TAG = "sm";

extern char g_device_sn[SN_MAX_LEN];
extern char g_device_key[65];

enum SystemState g_sys_state = STATE_INIT;

char g_wifi_ssid[33] = {0};
char g_wifi_pass[65] = {0};
static int64_t g_wifi_connect_start = 0;
static int g_wifi_reconnect_attempts = 0;
static int64_t g_wifi_reconnect_delay = 1000;
static int64_t g_wifi_retry_next = 0;

// g_last_heartbeat 已移除：status 不再周期性发送
static int64_t g_last_uart_heartbeat = 0;
static int64_t g_last_cache_flush = 0;
static int64_t g_last_debug = 0;

#define CMD_DEDUP_SLOTS 8
static char g_recent_task_ids[CMD_DEDUP_SLOTS][64];
static char g_recent_responses[CMD_DEDUP_SLOTS][FRAME_DATA_MAX];
static unsigned g_recent_task_next;
static bool g_recent_loaded;

static void load_recent_commands(void) {
    if (g_recent_loaded) return;
    for (unsigned i = 0; i < CMD_DEDUP_SLOTS; ++i) {
        storage_load_cmd_result(i, g_recent_task_ids[i], sizeof(g_recent_task_ids[i]),
                                g_recent_responses[i], sizeof(g_recent_responses[i]));
    }
    g_recent_task_next = storage_get_cmd_next();
    g_recent_loaded = true;
}

static bool command_seen(const char *task_id)
{
    if (!task_id || !task_id[0]) return false;
    load_recent_commands();
    for (unsigned i = 0; i < CMD_DEDUP_SLOTS; ++i) {
        if (strcmp(g_recent_task_ids[i], task_id) == 0) return true;
    }
    return false;
}

static bool remember_command(const char *task_id, const char *response)
{
    if (!task_id || !task_id[0] || !response) return false;
    load_recent_commands();
    unsigned slot = g_recent_task_next;
    for (unsigned i = 0; i < CMD_DEDUP_SLOTS; ++i) {
        if (strcmp(g_recent_task_ids[i], task_id) == 0) { slot = i; break; }
    }
    if (!storage_save_cmd_result(slot, task_id, response)) return false;
    strlcpy(g_recent_task_ids[slot], task_id, sizeof(g_recent_task_ids[slot]));
    strlcpy(g_recent_responses[slot], response, sizeof(g_recent_responses[slot]));
    if (slot == g_recent_task_next) {
        unsigned next = (g_recent_task_next + 1) % CMD_DEDUP_SLOTS;
        if (!storage_set_cmd_next(next)) return false;
        g_recent_task_next = next;
    }
    return true;
}

static void replay_command(const char* task_id) {
    load_recent_commands();
    for (unsigned i = 0; i < CMD_DEDUP_SLOTS; ++i) {
        if (strcmp(g_recent_task_ids[i], task_id) == 0 && g_recent_responses[i][0]) {
            mqtt_publish(PROTO_TOPIC_CMD_RESP, g_recent_responses[i],
                         strlen(g_recent_responses[i]), 1, 0);
            return;
        }
    }
}

static void publish_command_reply(const char *task_id, const char *cmd,
                                  const char *stage, bool success,
                                  const char *code, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "task_id", task_id ? task_id : "");
    cJSON_AddStringToObject(root, "cmd", cmd ? cmd : "");
    cJSON_AddStringToObject(root, "stage", stage);
    cJSON_AddBoolToObject(root, "success", success);
    cJSON_AddStringToObject(root, "code", code);
    cJSON_AddStringToObject(root, "message", message ? message : "");
    cJSON_AddItemToObject(root, "result", cJSON_CreateArray());
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json) {
        if (remember_command(task_id, json)) {
            mqtt_publish(PROTO_TOPIC_CMD_RESP, json, strlen(json), 1, 0);
        } else {
            char fallback[256];
            int n = snprintf(fallback, sizeof(fallback),
                "{\"task_id\":\"%s\",\"cmd\":\"%s\",\"stage\":\"done\","
                "\"success\":false,\"code\":\"PERSIST_FAILED\","
                "\"message\":\"Command result persistence failed\",\"result\":[]}",
                task_id ? task_id : "", cmd ? cmd : "");
            if (n > 0 && n < (int)sizeof(fallback)) {
                mqtt_publish(PROTO_TOPIC_CMD_RESP, fallback, n, 1, 0);
            }
        }
        free(json);
    }
}

/* ARM 数据超时看门告警 */
static int64_t g_last_data_received = 0;
static int64_t g_last_arm_alarm_check = 0;
static bool g_arm_alarm_sent = false;
#define ARM_DATA_TIMEOUT_MS   (10LL * 60 * 1000)  /* 10 分钟 */
#define ARM_ALARM_CHECK_MS    (30LL * 1000)        /* 每 30 秒检查一次 */
#define ARM_ALARM_CODE        13                    /* 告警码 */

static bool g_btn_pressed = false;
static int64_t g_btn_press_start = 0;

extern EventGroupHandle_t s_wifi_event_group;
extern const int WIFI_CONNECTED_BIT;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void init_wifi_sta(void)
{
    char ap_ssid[32];
    build_ap_ssid(ap_ssid, sizeof(ap_ssid));
    ESP_LOGI(TAG, "init_wifi_sta: ssid='%s', pass_len=%d", g_wifi_ssid, (int)strlen(g_wifi_pass));

    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(50));

    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode != WIFI_MODE_APSTA) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    esp_wifi_set_ps(WIFI_PS_NONE);

    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, g_wifi_ssid, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, g_wifi_pass, sizeof(sta_cfg.sta.password) - 1);
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);

    esp_wifi_connect();
    g_wifi_connect_start = now_ms();
    g_wifi_reconnect_attempts = 0;
    ESP_LOGI(TAG, "Connecting to %s...", g_wifi_ssid);
}

void build_ap_ssid(char *out, size_t out_len)
{
    // 1. 优先使用自定义 AP SSID
    char custom_ssid[AP_SSID_MAX_LEN];
    if (storage_get_ap_ssid(custom_ssid, sizeof(custom_ssid))) {
        size_t clen = strlen(custom_ssid);
        bool valid = (clen >= 2 && clen <= 31);
        if (valid) {
            for (size_t i = 0; i < clen; i++) {
                if (custom_ssid[i] < 0x20 || custom_ssid[i] > 0x7E) {
                    valid = false;
                    break;
                }
            }
        }
        if (valid) {
            strncpy(out, custom_ssid, out_len - 1);
            out[out_len - 1] = '\0';
            return;
        }
        storage_clear_ap_ssid();
    }

    // 2. 有 SN 时用 SN, 无 SN 时用 MAC 地址后 3 字节
    size_t prefix_len = strlen(AP_SSID_PREFIX);
    if (g_device_sn[0] != '\0') {
        size_t sn_len = strlen(g_device_sn);
        size_t max_sn = (out_len > prefix_len + 1) ? (out_len - prefix_len - 1) : 0;
        size_t copy_len = (sn_len < max_sn) ? sn_len : max_sn;
        memcpy(out, AP_SSID_PREFIX, prefix_len);
        memcpy(out + prefix_len, g_device_sn, copy_len);
        out[prefix_len + copy_len] = '\0';
        for (char *p = out + prefix_len; *p; p++) {
            if (*p >= 'a' && *p <= 'z') *p = *p - 'a' + 'A';
        }
    } else {
        // 无 SN, 用 MAC 地址后 3 字节: CS_INV_AABBCC
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(out, out_len, "%s%02X%02X%02X",
                 AP_SSID_PREFIX, mac[3], mac[4], mac[5]);
    }
}

static void state_sta_connecting(void)
{
    if (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) {
        esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        esp_netif_ip_info_t sta_ip = {0}, ap_ip = {0};
        if (sta_netif) esp_netif_get_ip_info(sta_netif, &sta_ip);
        if (ap_netif) esp_netif_get_ip_info(ap_netif, &ap_ip);
        ESP_LOGI(TAG, "Connected! STA=" IPSTR " AP=" IPSTR,
                 IP2STR(&sta_ip.ip), IP2STR(&ap_ip.ip));
        g_wifi_reconnect_attempts = 0;
        g_wifi_reconnect_delay = 1000;
        g_wifi_retry_next = 0;

        g_sys_state = STATE_MQTT_CONNECTING;
        led1_set(LED_SOLID);
        led2_set(LED_SLOW_BLINK);
        return;
    }

    int64_t now = now_ms();

    if (g_wifi_retry_next == 0 && now - g_wifi_connect_start > 15000) {
        g_wifi_reconnect_attempts++;
        ESP_LOGI(TAG, "Retry %d", g_wifi_reconnect_attempts);
        if (g_wifi_reconnect_attempts >= 10) {
            ESP_LOGW(TAG, "Max retries -> AP mode");
            esp_wifi_disconnect();
            esp_wifi_set_mode(WIFI_MODE_APSTA);

            char ap_ssid[32];
            build_ap_ssid(ap_ssid, sizeof(ap_ssid));
            wifi_config_t ap_cfg = {0};
            strncpy((char *)ap_cfg.ap.ssid, ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
            ap_cfg.ap.ssid_len = strlen(ap_ssid);
            ap_cfg.ap.max_connection = 1;
            ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
            esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);

            g_sys_state = STATE_AP_PROVISION;
            led1_set(LED_SLOW_BLINK);
            led2_set(LED_OFF);
            ap_api_server_start();
            g_wifi_retry_next = 0;
            return;
        }
        g_wifi_retry_next = now + g_wifi_reconnect_delay;
        ESP_LOGI(TAG, "Waiting %lld ms before retry...", g_wifi_reconnect_delay);
        g_wifi_reconnect_delay = (g_wifi_reconnect_delay * 2 < 60000) ? g_wifi_reconnect_delay * 2 : 60000;
        return;
    }

    if (g_wifi_retry_next > 0 && now < g_wifi_retry_next) {
        return;
    }

    if (g_wifi_retry_next > 0 && now >= g_wifi_retry_next) {
        g_wifi_retry_next = 0;
        esp_wifi_disconnect();

        wifi_config_t sta_cfg = {0};
        strncpy((char *)sta_cfg.sta.ssid, g_wifi_ssid, sizeof(sta_cfg.sta.ssid) - 1);
        strncpy((char *)sta_cfg.sta.password, g_wifi_pass, sizeof(sta_cfg.sta.password) - 1);
        esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);

        esp_wifi_connect();
        g_wifi_connect_start = now_ms();
    }
}

static void state_mqtt_connecting(void)
{
    if (!(xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT)) {
        init_wifi_sta();
        g_sys_state = STATE_STA_CONNECTING;
        led1_set(LED_FAST_BLINK);
        led2_set(LED_OFF);
        return;
    }
    if (mqtt_is_connected()) {
        ESP_LOGI(TAG, ">>> RUNNING <<<");
        g_sys_state = STATE_RUNNING;
        led1_set(LED_SOLID);
        led2_set(LED_SOLID);
        g_last_data_received = now_ms();
        g_arm_alarm_sent = false;
        return;
    }
    mqtt_connect();
}

static void state_running(void)
{
    if (!(xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "WiFi lost!");
        led1_set(LED_FAST_BLINK);
        led2_set(LED_OFF);
        mqtt_on_network_lost();
        init_wifi_sta();
        g_sys_state = STATE_STA_CONNECTING;
        return;
    }
    if (!mqtt_is_connected()) {
        led1_set(LED_SOLID);
        led2_set(LED_FAULT);
        g_sys_state = STATE_MQTT_CONNECTING;
        return;
    }

    // status 仅在上线/主动下线/LWT 时发布（MQTT keepalive 维护连接状态）

    if (now_ms() - g_last_cache_flush >= 10000) {
        g_last_cache_flush = now_ms();
        mqtt_flush_offline_cache();
    }

    /* ARM 数据超时看门告警（每 30 秒检查一次） */
    if (now_ms() - g_last_arm_alarm_check >= ARM_ALARM_CHECK_MS) {
        g_last_arm_alarm_check = now_ms();

        if (now_ms() - g_last_data_received > ARM_DATA_TIMEOUT_MS) {
            if (!g_arm_alarm_sent) {
                g_arm_alarm_sent = true;
                char alarm_msg[256];
                snprintf(alarm_msg, sizeof(alarm_msg),
                    "{\"source\":%d,\"code\":%d,\"level\":%d,\"state\":%d}",
                    PROTO_SOURCE_COMM, ARM_ALARM_CODE, PROTO_LEVEL_FAULT, PROTO_STATE_ACTIVE);
                mqtt_publish(PROTO_TOPIC_ALARM, alarm_msg, strlen(alarm_msg), 1, 0);
                ESP_LOGW(TAG, "ARM watchdog alarm: no data for %lld ms",
                         now_ms() - g_last_data_received);
            }
        } else if (g_arm_alarm_sent) {
            g_arm_alarm_sent = false;
            char recovery_msg[128];
            snprintf(recovery_msg, sizeof(recovery_msg),
                "{\"source\":%d,\"code\":%d,\"level\":%d,\"state\":%d}",
                PROTO_SOURCE_COMM, ARM_ALARM_CODE, PROTO_LEVEL_FAULT, PROTO_STATE_RECOVERED);
            mqtt_publish(PROTO_TOPIC_ALARM, recovery_msg, strlen(recovery_msg), 1, 0);
            ESP_LOGI(TAG, "ARM watchdog recovery: data resumed");
        }
    }
}

void state_machine_init(void)
{
    g_last_debug = now_ms();
    g_last_cache_flush = now_ms();
}

void state_machine_run(void)
{
    int64_t now = now_ms();

    if (now - g_last_debug >= 60000) {
        g_last_debug = now;
        int cached = offline_cache_count();
        ESP_LOGI(TAG, "State=%d, Cached=%d, Heap=%lu",
                 g_sys_state, cached, (unsigned long)esp_get_free_heap_size());
    }

    if (now - g_last_uart_heartbeat >= 10000) {
        g_last_uart_heartbeat = now;
        uart_send_frame(CMD_HEARTBEAT, NULL, 0);
        ESP_LOGI(TAG, "UART HEARTBEAT sent");
    }

    switch (g_sys_state) {
    case STATE_INIT:
        break;
    case STATE_AP_PROVISION:
        break;
    case STATE_STA_CONNECTING:
        state_sta_connecting();
        break;
    case STATE_MQTT_CONNECTING:
        state_mqtt_connecting();
        break;
    case STATE_RUNNING:
        state_running();
        mqtt_ota_process();
        break;
    case STATE_OTA:
        ota_service_process();
        break;
    }
}

static bool extract_str(const char *js, const char *key, char *out, int max_len)
{
    const char *p = strstr(js, key);
    if (!p) return false;
    p = strchr(p + strlen(key), ':');
    if (!p) return false;
    p = strchr(p + 1, '"');
    if (!p) return false;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < max_len - 1) {
        if (*p == '\\' && *(p + 1)) p++;
        out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}

void on_uart_frame(uint8_t cmd, const uint8_t *data, uint16_t len)
{
    switch (cmd) {
    case CMD_SET_BROKER: {
        char buf[128] = {0};
        if (len == 0 || len >= sizeof(buf)) {
            uart_send_nack();
            break;
        }
        memcpy(buf, data, len);
        char *colon = strrchr(buf, ':');
        bool accepted = false;
        if (colon && colon != buf && colon[1]) {
            *colon = '\0';
            errno = 0;
            char *end = NULL;
            const unsigned long port = strtoul(colon + 1, &end, 10);
            if (errno == 0 && end != colon + 1 && *end == '\0' && port <= UINT16_MAX) {
                accepted = mqtt_set_broker(buf, (uint16_t)port);
                if (accepted) {
                    ESP_LOGI(TAG, "Broker: %s:%lu", buf, port);
                    g_sys_state = STATE_MQTT_CONNECTING;
                }
            }
        }
        if (accepted) uart_send_ack(); else uart_send_nack();
        break;
    }

    case CMD_PUBLISH: {
        char json[FRAME_DATA_MAX];
        size_t copy_len = (len < sizeof(json) - 1) ? len : sizeof(json) - 1;
        memcpy(json, data, copy_len);
        json[copy_len] = '\0';

        char topic[64] = {0};
        char payload[FRAME_DATA_MAX - 80] = {0};
        char sn[SN_MAX_LEN] = {0};
        int publish_result = -1;

        extract_str(json, "\"topic\"", topic, sizeof(topic));
        extract_str(json, "\"payload\"", payload, sizeof(payload));
        if (extract_str(json, "\"sn\"", sn, sizeof(sn))) {
            if (strlen(sn) > 0 && strcmp(sn, g_device_sn) != 0) {
                if (!sn_validate(sn)) {
                    ESP_LOGW(TAG, "PUBLISH SN REJECTED (invalid): %s", sn);
                    uart_send_nack();
                    return;
                }
                if (!storage_set_sn(sn)) {
                    ESP_LOGE(TAG, "Failed to persist SN from PUBLISH");
                    uart_send_nack();
                    return;
                }
                strncpy(g_device_sn, sn, sizeof(g_device_sn) - 1);
                ESP_LOGI(TAG, "SN updated via PUBLISH: %s", sn);
                mqtt_reconfigure();
                if (g_sys_state == STATE_RUNNING) g_sys_state = STATE_MQTT_CONNECTING;

                char ap_ssid[32];
                build_ap_ssid(ap_ssid, sizeof(ap_ssid));
                wifi_config_t ap_cfg = {0};
                strncpy((char *)ap_cfg.ap.ssid, ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
                ap_cfg.ap.ssid_len = strlen(ap_ssid);
                ap_cfg.ap.max_connection = 1;
                ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
                esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
            }
        }

        if (strlen(topic) > 0) {
            const char *send_payload = payload;
            char final_payload[FRAME_DATA_MAX - 80];
            const char *esp_ver = FIRMWARE_VERSION;
            char *esp_pos = strstr(payload, "\"firmware_esp\"");
            if (esp_pos) {
                char *ver_start = strchr(esp_pos + 14, ':');
                if (ver_start) {
                    ver_start = strchr(ver_start + 1, '"');
                    if (ver_start) {
                        char *ver_end = strchr(ver_start + 1, '"');
                        if (ver_end) {
                            int prefix_len = ver_start + 1 - payload;
                            int suffix_len = strlen(ver_end);
                            int ver_len = strlen(esp_ver);
                            if (prefix_len + ver_len + suffix_len < (int)sizeof(final_payload)) {
                                memcpy(final_payload, payload, prefix_len);
                                memcpy(final_payload + prefix_len, esp_ver, ver_len);
                                strcpy(final_payload + prefix_len + ver_len, ver_end);
                                send_payload = final_payload;
                                ESP_LOGI(TAG, "Replaced firmware_esp with %s", esp_ver);
                            }
                        }
                    }
                }
            }
            int qos = strcmp(topic, PROTO_TOPIC_THREE_PHASE) == 0 ? 0 : 1;
            publish_result = mqtt_publish(topic, send_payload, strlen(send_payload), qos, 0);
            g_last_data_received = now_ms();
            led_signal_data();
            ESP_LOGI(TAG, "PUBLISH -> cs_inv/%s/%s (%d bytes)",
                     g_device_sn, topic, (int)strlen(send_payload));
        } else {
            ESP_LOGW(TAG, "PUBLISH error: topic empty");
        }
        if (strlen(topic) > 0 && publish_result >= 0) uart_send_ack();
        else uart_send_nack();
        break;
    }

    case CMD_SET_SN: {
        char sn[SN_MAX_LEN] = {0};
        memcpy(sn, data, MIN((size_t)len, sizeof(sn) - 1));
        ESP_LOGI(TAG, "SN received from ARM: %s", sn);

        bool changed = (strcmp(g_device_sn, sn) != 0);
        if (!storage_set_sn(sn)) {
            ESP_LOGE(TAG, "Failed to persist SN");
            uart_send_nack();
            break;
        }
        strncpy(g_device_sn, sn, sizeof(g_device_sn) - 1);
        uart_send_ack();
        if (changed) {
            ESP_LOGI(TAG, "SN changed to %s, restarting...", sn);
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        } else {
            ESP_LOGI(TAG, "SN=%s (no change)", sn);
        }
        break;
    }

    case CMD_SET_KEY: {
        char key[65] = {0};
        if (len == 0 || len >= sizeof(key)) {
            uart_send_nack();
            break;
        }
        memcpy(key, data, len);
        if (!storage_set_key(key)) {
            ESP_LOGE(TAG, "Failed to persist MQTT key");
            uart_send_nack();
            break;
        }
        strncpy(g_device_key, key, sizeof(g_device_key) - 1);
        mqtt_reconfigure();
        if (s_wifi_event_group &&
            (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT)) {
            g_sys_state = STATE_MQTT_CONNECTING;
        }
        uart_send_ack();
        break;
    }

    case CMD_SET_AP_SSID: {
        char ap_ssid[AP_SSID_MAX_LEN] = {0};
        if (len >= sizeof(ap_ssid)) {
            uart_send_nack();
            break;
        }
        memcpy(ap_ssid, data, len);
        bool saved;
        if (strlen(ap_ssid) == 0) {
            saved = storage_clear_ap_ssid();
            if (saved) ESP_LOGI(TAG, "AP SSID reset to default");
        } else {
            saved = storage_set_ap_ssid(ap_ssid);
            if (saved) ESP_LOGI(TAG, "AP SSID set to: %s", ap_ssid);
        }
        if (!saved) {
            uart_send_nack();
            break;
        }
        uart_send_ack();
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
        break;
    }

    case CMD_FACTORY_RESET:
        offline_cache_clear();
        storage_factory_reset();
        uart_send_ack();
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
        break;

    case CMD_ARM_OTA_ACK:
    case CMD_ARM_OTA_NACK:
    case CMD_ARM_OTA_INFO:
        arm_ota_handle_uart(cmd, data, len);
        break;

    case CMD_COMMAND_SEND: {
        char json[FRAME_DATA_MAX];
        size_t copy_len = (len < sizeof(json) - 1) ? len : sizeof(json) - 1;
        memcpy(json, data, copy_len);
        json[copy_len] = '\0';
        bool persisted = false;
        cJSON* result = cJSON_Parse(json);
        if (result) {
            cJSON* task = cJSON_GetObjectItemCaseSensitive(result, "task_id");
            if (cJSON_IsString(task)) persisted = remember_command(task->valuestring, json);
            cJSON_Delete(result);
        }
        int publish_result = mqtt_publish(PROTO_TOPIC_CMD_RESP, json, copy_len, 1, 0);
        if (persisted && publish_result >= 0) {
            ESP_LOGI(TAG, "ARM command result accepted: result=%d", publish_result);
            uart_send_ack();
        } else {
            ESP_LOGE(TAG, "ARM command result publish/cache failed: %d", publish_result);
            uart_send_nack();
        }
        break;
    }

    default:
        uart_send_nack();
        break;
    }
}

static bool apply_provision(const char *ssid, const char *password)
{
    if (!ssid || !password || !ssid[0] || strlen(ssid) > 32 || strlen(password) > 64) {
        return false;
    }
    ESP_LOGI(TAG, "Provision: %s, pass_len=%d", ssid, (int)strlen(password));
    if (!storage_set_wifi(ssid, password)) {
        ESP_LOGE(TAG, "Failed to persist Wi-Fi configuration");
        return false;
    }
    strncpy(g_wifi_ssid, ssid, sizeof(g_wifi_ssid) - 1);
    strncpy(g_wifi_pass, password, sizeof(g_wifi_pass) - 1);
    init_wifi_sta();
    g_sys_state = STATE_STA_CONNECTING;
    led1_set(LED_FAST_BLINK);
    led2_set(LED_OFF);
    ESP_LOGI(TAG, "Connecting STA to %s", ssid);
    return true;
}

void on_provision_done(const char *ssid, const char *password)
{
    if (apply_provision(ssid, password)) {
        if (!ap_api_server_start()) {
            ESP_LOGE(TAG, "Failed to restore App API after BLE provisioning");
        }
    }
}

bool on_ap_provision_done(const char *ssid, const char *password)
{
    return apply_provision(ssid, password);
}

void on_mqtt_message(const char *topic, const char *payload, unsigned int len)
{
    const char *sub = strrchr(topic, '/');
    if (sub) sub++; else sub = topic;

    if (strstr(topic, "/ota/cmd") != NULL) {
        ESP_LOGI(TAG, "OTA cmd topic=%s len=%u", topic, len);
        mqtt_ota_handle_command(topic, (const uint8_t *)payload, len);
        return;
    }

    /* ---- /cmd 下行命令：提取 task_id / t，过期校验，回复 ACK ---- */
    bool is_cmd_topic = (strcmp(sub, "cmd") == 0);
    if (is_cmd_topic) {
        if (len == 0 || len > FRAME_DATA_MAX || len > UINT16_MAX) {
            publish_command_reply("", "", PROTO_CMD_STAGE_DONE, false,
                                  PROTO_ERR_INVALID_ARGS, "Command exceeds UART frame limit");
            return;
        }
        cJSON *root = cJSON_ParseWithLength(payload, len);
        if (!root) {
            publish_command_reply("", "", PROTO_CMD_STAGE_DONE, false,
                                  PROTO_ERR_INVALID_ARGS, "Invalid JSON");
            return;
        }
        const cJSON *task = cJSON_GetObjectItemCaseSensitive(root, "task_id");
        const cJSON *cmd_item = cJSON_GetObjectItemCaseSensitive(root, "cmd");
        const cJSON *timestamp = cJSON_GetObjectItemCaseSensitive(root, "t");
        const char *task_id = cJSON_IsString(task) ? task->valuestring : "";
        const char *cmd_str = cJSON_IsString(cmd_item) ? cmd_item->valuestring : "";
        bool valid_t = cJSON_IsNumber(timestamp) && isfinite(timestamp->valuedouble) &&
                       timestamp->valuedouble > 0 && timestamp->valuedouble <= UINT32_MAX &&
                       floor(timestamp->valuedouble) == timestamp->valuedouble;
        time_t cmd_t = valid_t ? (time_t)timestamp->valuedouble : 0;
        time_t now = time(NULL);

        if (!task_id[0] || !cmd_str[0] || !valid_t) {
            publish_command_reply(task_id, cmd_str, PROTO_CMD_STAGE_DONE, false,
                                  PROTO_ERR_INVALID_ARGS,
                                  "task_id, cmd and numeric t are required");
            cJSON_Delete(root);
            return;
        }

        if (command_seen(task_id)) {
            replay_command(task_id);
            cJSON_Delete(root);
            return;
        }
        if (now >= 1704067200 && (now - cmd_t) > PROTO_CMD_EXPIRE_S) {
            publish_command_reply(task_id, cmd_str, PROTO_CMD_STAGE_DONE, false,
                                  PROTO_ERR_EXPIRED, "Command expired");
            cJSON_Delete(root);
            return;
        }
        if (!uart_send_frame(CMD_COMMAND_RECV, (const uint8_t *)payload, (uint16_t)len)) {
            publish_command_reply(task_id, cmd_str, PROTO_CMD_STAGE_DONE, false,
                                  PROTO_ERR_EXEC_FAILED, "UART delivery failed");
            cJSON_Delete(root);
            return;
        }
        publish_command_reply(task_id, cmd_str, PROTO_CMD_STAGE_ACK, true,
                              PROTO_ERR_OK, "");
        cJSON_Delete(root);
        return;

    }

    /* 转发给 ARM */
    if (len == 0 || len > FRAME_DATA_MAX || len > UINT16_MAX) {
        ESP_LOGE(TAG, "MQTT payload cannot fit UART frame: %u", len);
        return;
    }
    uart_send_frame(CMD_COMMAND_RECV, (const uint8_t *)payload, (uint16_t)len);
    ESP_LOGI(TAG, "MQTT->ARM topic=%s len=%u", sub, len);
}

void check_button(void)
{
    int level = gpio_get_level(PIN_BTN);
    bool pressed = (level == 0);
    int64_t now = now_ms();

    if (pressed && !g_btn_pressed) {
        g_btn_pressed = true;
        g_btn_press_start = now;
    }
    if (!pressed && g_btn_pressed) {
        g_btn_pressed = false;
        int64_t duration = now - g_btn_press_start;
        if (duration >= BUTTON_LONG_PRESS) {
            ESP_LOGW(TAG, "Factory reset!");
            led1_set(LED_3FLASH);
            led2_set(LED_3FLASH);
            offline_cache_clear();
            storage_factory_reset();
            vTaskDelay(pdMS_TO_TICKS(1500));
            esp_restart();
        } else if (duration > 200 && duration < BUTTON_LONG_PRESS) {
            // 短按：无操作或LED指示
            ESP_LOGI(TAG, "Short press detected");
        }
    }
}
