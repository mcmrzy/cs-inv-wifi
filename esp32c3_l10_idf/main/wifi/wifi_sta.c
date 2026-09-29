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
static int s_disconnect_count = 0;
#define WIFI_RESET_AFTER_FAILS  5

static esp_err_t set_dns_server(esp_netif_t *netif, esp_netif_dns_type_t type,
                                uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(a, b, c, d);
    return esp_netif_set_dns_info(netif, type, &dns);
}

static void configure_dns_servers(esp_netif_t *netif)
{
    esp_netif_dns_info_t dhcp_dns = {0};
    esp_err_t err = esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN,
                                           &dhcp_dns);

    /* Preserve the DHCP-provided resolver as the final fallback. The field
     * installation routers used on site advertise their gateway here, but
     * that gateway returned getaddrinfo(EAI_FAIL) for the MQTT hostname. */
    if (err == ESP_OK && dhcp_dns.ip.type == ESP_IPADDR_TYPE_V4 &&
        dhcp_dns.ip.u_addr.ip4.addr != 0) {
        err = esp_netif_set_dns_info(netif, ESP_NETIF_DNS_FALLBACK,
                                     &dhcp_dns);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Preserve DHCP DNS as fallback failed: %s",
                     esp_err_to_name(err));
        }
    }

    /* Use two independent public resolvers before marking WiFi connected, so
     * the MQTT task never starts with only an unusable gateway resolver. */
    err = set_dns_server(netif, ESP_NETIF_DNS_MAIN, 223, 5, 5, 5);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Set primary DNS failed: %s", esp_err_to_name(err));
    }
    err = set_dns_server(netif, ESP_NETIF_DNS_BACKUP, 119, 29, 29, 29);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Set backup DNS failed: %s", esp_err_to_name(err));
    }
}

static void log_dns_servers(esp_netif_t *netif)
{
    static const struct {
        esp_netif_dns_type_t type;
        const char *name;
    } servers[] = {
        {ESP_NETIF_DNS_MAIN, "main"},
        {ESP_NETIF_DNS_BACKUP, "backup"},
        {ESP_NETIF_DNS_FALLBACK, "fallback"},
    };

    for (size_t i = 0; i < sizeof(servers) / sizeof(servers[0]); ++i) {
        esp_netif_dns_info_t dns = {0};
        esp_err_t err = esp_netif_get_dns_info(netif, servers[i].type, &dns);
        if (err == ESP_OK && dns.ip.type == ESP_IPADDR_TYPE_V4) {
            ESP_LOGI(TAG, "DNS %s: " IPSTR, servers[i].name,
                     IP2STR(&dns.ip.u_addr.ip4));
        } else {
            ESP_LOGW(TAG, "DNS %s unavailable: %s", servers[i].name,
                     esp_err_to_name(err));
        }
    }
}

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
            s_disconnect_count++;
            ESP_LOGW(TAG, "STA disconnected, reason=%u (count=%d)",
                     evt->reason, s_disconnect_count);
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            if (s_disconnect_count >= WIFI_RESET_AFTER_FAILS) {
                ESP_LOGW(TAG, "Too many disconnects, resetting WiFi");
                esp_wifi_stop();
                esp_wifi_start();
                s_disconnect_count = 0;
                return;    /* esp_wifi_start 后 auto-reconnect 生效 */
            }
            esp_wifi_connect();
            break;
        }
        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *evt = event_data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&evt->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", s_ip_str);
        ESP_LOGI(TAG, "Netif: %s", esp_netif_get_desc(evt->esp_netif));
        configure_dns_servers(evt->esp_netif);
        log_dns_servers(evt->esp_netif);
        s_disconnect_count = 0;
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
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    /* 先设新配置, 再断开: 断开事件的 auto-reconnect 直接连新 AP,
       避免先断开→重连旧 AP→再切新 AP 的双倍耗时 */
    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "STA config failed: %s", esp_err_to_name(ret));
        return false;
    }

    /* 用驱动真实状态判断，event group bit 可能已被上面 clear */
    wifi_ap_record_t ap_info;
    bool driver_connected = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);

    if (wifi_sta_is_connected() || driver_connected) {
        /* 已连接: 先 disconnect，STA_DISCONNECTED 回调会用新配置 auto-reconnect */
        esp_wifi_disconnect();
        ESP_LOGI(TAG, "reconnecting to SSID=%s", ssid);
    } else {
        esp_err_t cr = esp_wifi_connect();
        if (cr != ESP_OK && cr != ESP_ERR_WIFI_NOT_STARTED) {
            ESP_LOGW(TAG, "connect failed: %s", esp_err_to_name(cr));
            return false;
        }
        ESP_LOGI(TAG, "connecting to SSID=%s", ssid);
    }
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
