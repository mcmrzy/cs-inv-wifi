/*
 * wifi_sta.c — WiFi STA + 自动重连
 *
 * 断开后依赖 esp_wifi 内置 auto-reconnect; IP 获取后置 WIFI_CONNECTED_BIT
 */
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "config.h"
#include "wifi_sta.h"

static const char *TAG = "WIFI";

EventGroupHandle_t s_wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;

static char s_ip_str[32] = "0.0.0.0";
static uint8_t s_last_disconnect_reason = 0;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA started");
            break;
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "STA connected");
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *evt = event_data;
            s_last_disconnect_reason = evt->reason;
            ESP_LOGW(TAG, "STA disconnected, reason=%u", evt->reason);
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            /* esp_wifi 自动重连 (CONFIG_ESP_WIFI_ENABLE_WPA3_SAE 等默认开启 auto-reconnect) */
            break;
        }
        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *evt = event_data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&evt->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", s_ip_str);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_sta_init(void)
{
    if (s_wifi_event_group) {
        return;    /* 已初始化 */
    }
    s_wifi_event_group = xEventGroupCreate();

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                               wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                               wifi_event_handler, NULL);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "init done");
}

bool wifi_sta_connect(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) {
        return false;
    }

    /* 新连接开始: 清除上次连接状态, 保证 wait_connected 语义 */
    if (s_wifi_event_group) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }

    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid) - 1);
    if (pass && pass[0]) {
        strncpy((char *)sta_cfg.sta.password, pass,
                sizeof(sta_cfg.sta.password) - 1);
    }
    /* 自动重连 (默认开启, 显式设置一次) */
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "STA config failed: %s", esp_err_to_name(ret));
        return false;
    }
    ret = esp_wifi_connect();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "connect failed: %s", esp_err_to_name(ret));
        return false;
    }
    ESP_LOGI(TAG, "connecting to SSID=%s", ssid);
    return true;
}

bool wifi_sta_is_connected(void)
{
    return s_wifi_event_group &&
           (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT);
}

void wifi_sta_force_disconnect(void)
{
    esp_wifi_disconnect();
}

uint8_t wifi_sta_get_last_disconnect_reason(void)
{
    return s_last_disconnect_reason;
}

bool wifi_sta_wait_connected(uint32_t timeout_ms)
{
    if (!s_wifi_event_group) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
        timeout_ms ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

int wifi_sta_get_rssi(void)
{
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return 0;
}

bool wifi_sta_get_ip(char *ip, size_t len)
{
    if (!ip || len == 0) {
        return false;
    }
    if (!wifi_sta_is_connected()) {
        return false;
    }
    snprintf(ip, len, "%s", s_ip_str);
    return true;
}
