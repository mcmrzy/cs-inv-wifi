#include "state_machine.h"
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

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>

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

static int64_t g_last_heartbeat = 0;
static int64_t g_last_uart_heartbeat = 0;
static int64_t g_last_cache_flush = 0;
static int64_t g_last_debug = 0;

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

    size_t sn_len = strlen(g_device_sn);
    size_t prefix_len = strlen(AP_SSID_PREFIX);
    size_t max_sn = (out_len > prefix_len + 1) ? (out_len - prefix_len - 1) : 0;
    size_t copy_len = (sn_len < max_sn) ? sn_len : max_sn;
    memcpy(out, AP_SSID_PREFIX, prefix_len);
    memcpy(out + prefix_len, g_device_sn, copy_len);
    out[prefix_len + copy_len] = '\0';
    for (char *p = out + prefix_len; *p; p++) {
        if (*p >= 'a' && *p <= 'z') *p = *p - 'a' + 'A';
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
            tcp_provision_init();
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
        g_last_heartbeat = now_ms();
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
        mqtt_disconnect();
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

    if (now_ms() - g_last_heartbeat >= HEARTBEAT_INTERVAL) {
        g_last_heartbeat = now_ms();

        esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip_info = {0};
        if (sta_netif) esp_netif_get_ip_info(sta_netif, &ip_info);

        wifi_ap_record_t ap_info = {0};
        esp_wifi_sta_get_ap_info(&ap_info);

        char status_msg[256];
        snprintf(status_msg, sizeof(status_msg),
                 "{\"online\":true,\"rssi\":%d,\"ip\":\"" IPSTR "\"}",
                 ap_info.rssi, IP2STR(&ip_info.ip));
        mqtt_publish(PROTO_TOPIC_STATUS, status_msg, strlen(status_msg), 1, 1);
        ESP_LOGI(TAG, "Heartbeat sent");
    }

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
                mqtt_publish(PROTO_TOPIC_ALARM, alarm_msg, strlen(alarm_msg), 0, 0);
                ESP_LOGW(TAG, "ARM watchdog alarm: no data for %lld ms",
                         now_ms() - g_last_data_received);
            }
        } else if (g_arm_alarm_sent) {
            g_arm_alarm_sent = false;
            char recovery_msg[128];
            snprintf(recovery_msg, sizeof(recovery_msg),
                "{\"source\":%d,\"code\":%d,\"level\":%d,\"state\":%d}",
                PROTO_SOURCE_COMM, ARM_ALARM_CODE, PROTO_LEVEL_FAULT, PROTO_STATE_RECOVERED);
            mqtt_publish(PROTO_TOPIC_ALARM, recovery_msg, strlen(recovery_msg), 0, 0);
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
        mqtt_ota_process();
        break;
    case STATE_OTA:
        ota_manager_process();
        tcp_provision_process();
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
        memcpy(buf, data, MIN((size_t)len, sizeof(buf) - 1));
        char *colon = strchr(buf, ':');
        if (colon) {
            *colon = '\0';
            storage_set_mqtt(buf, (uint16_t)atoi(colon + 1));
            ESP_LOGI(TAG, "Broker: %s:%s", buf, colon + 1);
            mqtt_disconnect();
            mqtt_init(on_mqtt_message);
            g_sys_state = STATE_MQTT_CONNECTING;
        }
        uart_send_ack();
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

        extract_str(json, "\"topic\"", topic, sizeof(topic));
        extract_str(json, "\"payload\"", payload, sizeof(payload));
        if (extract_str(json, "\"sn\"", sn, sizeof(sn))) {
            if (strlen(sn) > 0 && strcmp(sn, g_device_sn) != 0) {
                if (!sn_validate(sn)) {
                    ESP_LOGW(TAG, "PUBLISH SN REJECTED (invalid): %s", sn);
                    uart_send_nack();
                    return;
                }
                storage_set_sn(sn);
                strncpy(g_device_sn, sn, sizeof(g_device_sn) - 1);
                ESP_LOGI(TAG, "SN updated via PUBLISH: %s", sn);
                mqtt_disconnect();
                mqtt_init(on_mqtt_message);
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
            const char *esp_ver = ota_get_esp_version();
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
            mqtt_publish(topic, send_payload, strlen(send_payload), 0, 0);
            g_last_data_received = now_ms();
            led_signal_data();
            ESP_LOGI(TAG, "PUBLISH -> cs_inv/%s/%s (%d bytes)",
                     g_device_sn, topic, (int)strlen(send_payload));
        } else {
            ESP_LOGW(TAG, "PUBLISH error: topic empty");
        }
        uart_send_ack();
        break;
    }

    case CMD_SET_SN: {
        char sn[SN_MAX_LEN] = {0};
        memcpy(sn, data, MIN((size_t)len, sizeof(sn) - 1));

        if (!sn_validate(sn)) {
            ESP_LOGW(TAG, "SN REJECTED (invalid): %s", sn);
            uart_send_nack();
            break;
        }

        SNFields sf;
        if (sn_parse(sn, &sf)) {
            char readable[32];
            sn_printable(sn, readable, sizeof(readable));
            int year = sn_year_code_decode(sf.date_code[0]);
            char month = sf.date_code[1];
            ESP_LOGI(TAG, "SN validated: %s | Mfr=%s Country=%s Cust=%s "
                     "Year=%d Month=%c Seq=%s Check=%c",
                     readable, sf.manufacturer, sf.country, sf.customer,
                     year, month, sf.sequence, sf.check_digit);
        }

        bool changed = (strcmp(g_device_sn, sn) != 0);
        storage_set_sn(sn);
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
        memcpy(key, data, MIN((size_t)len, sizeof(key) - 1));
        storage_set_key(key);
        strncpy(g_device_key, key, sizeof(g_device_key) - 1);
        uart_send_ack();
        break;
    }

    case CMD_SET_AP_SSID: {
        char ap_ssid[AP_SSID_MAX_LEN] = {0};
        memcpy(ap_ssid, data, MIN((size_t)len, sizeof(ap_ssid) - 1));
        if (strlen(ap_ssid) == 0) {
            storage_clear_ap_ssid();
            ESP_LOGI(TAG, "AP SSID reset to default");
        } else {
            storage_set_ap_ssid(ap_ssid);
            ESP_LOGI(TAG, "AP SSID set to: %s", ap_ssid);
        }
        uart_send_ack();
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
        break;
    }

    case CMD_FACTORY_RESET:
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
        mqtt_publish(PROTO_TOPIC_CMD_RESP, json, copy_len, 1, 0);
        ESP_LOGI(TAG, "ARM->MQTT cmd_result: %s", json);
        uart_send_ack();
        break;
    }

    default:
        uart_send_nack();
        break;
    }
}

void on_provision_done(const char *ssid, const char *password)
{
    ESP_LOGI(TAG, "Provision: %s, pass_len=%d", ssid, (int)strlen(password));
    storage_set_wifi(ssid, password);
    strncpy(g_wifi_ssid, ssid, sizeof(g_wifi_ssid) - 1);
    strncpy(g_wifi_pass, password, sizeof(g_wifi_pass) - 1);
    init_wifi_sta();
    g_sys_state = STATE_STA_CONNECTING;
    led1_set(LED_FAST_BLINK);
    led2_set(LED_OFF);
    ESP_LOGI(TAG, "Connecting STA to %s", ssid);
}

void on_mqtt_message(const char *topic, const char *payload, unsigned int len)
{
    const char *sub = strrchr(topic, '/');
    if (sub) sub++; else sub = topic;

    if (strstr(topic, "/ota/cmd") != NULL) {
        ESP_LOGI(TAG, "OTA cmd topic=%s len=%u", topic, len);
        if (mqtt_ota_handle_command(topic, (const uint8_t *)payload, len)) {
            return;
        }
    }

    /* ---- /cmd 下行命令：提取 task_id / t，过期校验，回复 ACK ---- */
    bool is_cmd_topic = (strcmp(sub, "cmd") == 0);
    if (is_cmd_topic) {
        char task_id[72] = {0};
        char t_str[16]   = {0};
        char cmd_str[32] = {0};
        extract_str(payload, "\"task_id\"", task_id, sizeof(task_id));
        extract_str(payload, "\"t\"",       t_str,   sizeof(t_str));
        extract_str(payload, "\"cmd\"",     cmd_str, sizeof(cmd_str));
        time_t cmd_t = atol(t_str);
        time_t now   = time(NULL);

        /* 过期校验（NTP 未同步时跳过） */
        if (now > 0 && cmd_t > 0 && (now - cmd_t) > PROTO_CMD_EXPIRE_S) {
            char expired_resp[256];
            snprintf(expired_resp, sizeof(expired_resp),
                "{\"task_id\":\"%s\",\"cmd\":\"%s\",\"stage\":\"%s\","
                "\"success\":false,\"code\":\"%s\",\"message\":\"Command expired\",\"result\":[]}",
                task_id, cmd_str, PROTO_CMD_STAGE_DONE, PROTO_ERR_EXPIRED);
            mqtt_publish(PROTO_TOPIC_CMD_RESP, expired_resp, (int)strlen(expired_resp), 0, 0);
            ESP_LOGW(TAG, "CMD expired: task_id=%s cmd=%s t=%ld now=%ld",
                     task_id, cmd_str, (long)cmd_t, (long)now);
            return;  /* 不转发给 ARM */
        }

        /* 立即回复 acknowledged（QoS 0 低延迟） */
        if (task_id[0] != '\0') {
            char ack_resp[256];
            snprintf(ack_resp, sizeof(ack_resp),
                "{\"task_id\":\"%s\",\"cmd\":\"%s\",\"stage\":\"%s\","
                "\"success\":true,\"code\":\"%s\",\"message\":\"\",\"result\":[]}",
                task_id, cmd_str, PROTO_CMD_STAGE_ACK, PROTO_ERR_OK);
            mqtt_publish(PROTO_TOPIC_CMD_RESP, ack_resp, (int)strlen(ack_resp), 0, 0);
            ESP_LOGI(TAG, "CMD ACK sent: task_id=%s cmd=%s", task_id, cmd_str);
        }
    }

    /* 转发给 ARM */
    char json[512];
    int jslen = snprintf(json, sizeof(json),
                         "{\"topic\":\"%s\",\"payload\":\"%.*s\"}",
                         sub, (int)len, payload);
    uart_send_frame(CMD_COMMAND_RECV, (const uint8_t *)json, jslen);
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
            storage_factory_reset();
            vTaskDelay(pdMS_TO_TICKS(1500));
            esp_restart();
        } else if (duration > 200 && duration < BUTTON_LONG_PRESS) {
            ESP_LOGI(TAG, "Toggle AP mode");
            if (g_sys_state == STATE_AP_PROVISION) {
                if (storage_has_wifi_config()) {
                    storage_get_wifi(g_wifi_ssid, sizeof(g_wifi_ssid), g_wifi_pass, sizeof(g_wifi_pass));
                    init_wifi_sta();
                    g_sys_state = STATE_STA_CONNECTING;
                    led1_set(LED_FAST_BLINK);
                    led2_set(LED_OFF);
                }
            } else {
                esp_wifi_set_mode(WIFI_MODE_APSTA);

                char ap_ssid[32];
                build_ap_ssid(ap_ssid, sizeof(ap_ssid));
                wifi_config_t ap_cfg = {0};
                strncpy((char *)ap_cfg.ap.ssid, ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
                ap_cfg.ap.ssid_len = strlen(ap_ssid);
                ap_cfg.ap.max_connection = 1;
                ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
                esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);

                ESP_LOGI(TAG, "AP(%s) started alongside STA", ap_ssid);
                tcp_provision_init();
            }
        }
    }
}
