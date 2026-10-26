/*
 * wifi_sta.c — WiFi STA + 自动重连
 *
 * 断开事件驱动重连；同一时刻只提交一次连接请求。
 */
#include <string.h>
#include <stdatomic.h>
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
static atomic_bool s_connect_requested;
static atomic_bool s_driver_started;
static atomic_bool s_connect_inflight;
static atomic_bool s_restart_needed;
#define WIFI_RESET_AFTER_FAILS  5

static bool wifi_connect_if_needed(void)
{
    if (!atomic_load(&s_connect_requested) || !atomic_load(&s_driver_started)) {
        return true;
    }
    /* STA_START may preempt the caller after it publishes connection intent.
     * Both paths must claim the same attempt before entering the WiFi driver. */
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_connect_inflight, &expected, true)) {
        return true;
    }
    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        atomic_store(&s_connect_inflight, false);
        if (ret != ESP_ERR_WIFI_NOT_STARTED) {
            ESP_LOGW(TAG, "WiFi connect request failed: %s", esp_err_to_name(ret));
            return false;
        }
    }
    return true;
}

static void wifi_remember_channel(void)
{
    wifi_ap_record_t ap;
    wifi_config_t config;
    if (!atomic_load(&s_connect_requested) ||
        esp_wifi_sta_get_ap_info(&ap) != ESP_OK ||
        esp_wifi_get_config(WIFI_IF_STA, &config) != ESP_OK ||
        memcmp(ap.ssid, config.sta.ssid, sizeof(config.sta.ssid)) != 0 ||
        ap.primary == 0 || ap.primary > 14) {
        return;
    }
    if (config.sta.channel == ap.primary && !config.sta.bssid_set) {
        return;
    }
    /* IDF persists this configuration in its existing NVS namespace. A channel
     * is only a scan-order hint: IDF still searches the other channels if the
     * router moves. Never pin its BSSID, so replacement APs remain usable. */
    config.sta.channel = ap.primary;
    config.sta.bssid_set = false;
    memset(config.sta.bssid, 0, sizeof(config.sta.bssid));
    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Save WiFi channel hint failed: %s", esp_err_to_name(ret));
    }
}

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
            atomic_store(&s_restart_needed,false);
            atomic_store(&s_driver_started,true);
            ESP_LOGI(TAG, "STA started");
            /* start() only starts the driver. Reconnect intent survives a
             * failure-triggered stop/start; initial startup waits for config. */
            wifi_connect_if_needed();
            break;
        case WIFI_EVENT_STA_STOP:
            atomic_store(&s_driver_started,false);
            atomic_store(&s_connect_inflight,false);
            xEventGroupClearBits(s_wifi_event_group,WIFI_CONNECTED_BIT);
            break;
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGW(TAG, "STA associated; waiting for DHCP IP");
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *evt = event_data;
            s_last_disconnect_reason = evt->reason;
            atomic_store(&s_connect_inflight,false);
            s_disconnect_count++;
            ESP_LOGW(TAG, "STA disconnected, reason=%u (count=%d)",
                     evt->reason, s_disconnect_count);
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            if(!atomic_load(&s_connect_requested)){
                s_disconnect_count=0;
                break;
            }
            if (s_disconnect_count >= WIFI_RESET_AFTER_FAILS) {
                ESP_LOGW(TAG, "Too many disconnects, resetting WiFi");
                esp_wifi_stop();
                atomic_store(&s_driver_started,false);
                esp_err_t ret=esp_wifi_start();
                if(ret!=ESP_OK)ESP_LOGW(TAG,"WiFi restart failed: %s",esp_err_to_name(ret));
                s_disconnect_count = 0;
                return;    /* STA_START reconnects the current configured AP. */
            }
            wifi_connect_if_needed();
            break;
        }
        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        /* A DHCP event already queued before an explicit disconnect/stop must
         * not restore the connected bit or restart consumers afterwards. */
        if (!atomic_load(&s_connect_requested) || !atomic_load(&s_driver_started)) {
            return;
        }
        const ip_event_got_ip_t *evt = event_data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&evt->ip_info.ip));
        ESP_LOGW(TAG, "Got IP: %s", s_ip_str);
        ESP_LOGI(TAG, "Netif: %s", esp_netif_get_desc(evt->esp_netif));
        configure_dns_servers(evt->esp_netif);
        log_dns_servers(evt->esp_netif);
        s_disconnect_count = 0;
        wifi_remember_channel();
        if (atomic_load(&s_connect_requested) && atomic_load(&s_driver_started)) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
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

    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid) - 1);
    if (pass && pass[0]) {
        strncpy((char *)sta_cfg.sta.password, pass,
                sizeof(sta_cfg.sta.password) - 1);
    }
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    wifi_config_t previous;
    bool same_credentials = esp_wifi_get_config(WIFI_IF_STA, &previous) == ESP_OK &&
        memcmp(previous.sta.ssid, sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid)) == 0 &&
        memcmp(previous.sta.password, sta_cfg.sta.password, sizeof(sta_cfg.sta.password)) == 0;
    if (same_credentials && atomic_load(&s_connect_requested)) {
        /* Repeated setup/provision callbacks must not restart a live attempt
         * or clear a valid IP while DHCP/MQTT are already progressing. */
        return wifi_connect_if_needed();
    }
    if (same_credentials && previous.sta.channel <= 14) {
        sta_cfg.sta.channel = previous.sta.channel;
    }

    /* Keep the live connection if configuration is rejected for another
     * reason. IDF forbids set_config while searching for an unavailable AP;
     * stop that attempt before retrying with the replacement credentials. */
    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (ret == ESP_ERR_WIFI_STATE) {
        atomic_store(&s_connect_requested, false);
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        s_disconnect_count = 0;
        ret = esp_wifi_stop();
        if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED) {
            ESP_LOGE(TAG, "Stop old WiFi attempt failed: %s", esp_err_to_name(ret));
            return false;
        }
        atomic_store(&s_driver_started, false);
        atomic_store(&s_connect_inflight, false);
        esp_err_t configured = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        atomic_store(&s_connect_requested, configured == ESP_OK);
        /* STOP/DISCONNECTED events from the old attempt precede STA_START.
         * Leave driver_started false so only START can connect the new AP. */
        ret = esp_wifi_start();
        atomic_store(&s_restart_needed, ret != ESP_OK);
        if (configured != ESP_OK || ret != ESP_OK) {
            atomic_store(&s_connect_requested, false);
            ESP_LOGE(TAG, "Replace WiFi config/start failed: %s / %s",
                     esp_err_to_name(configured), esp_err_to_name(ret));
            return false;
        }
        ESP_LOGW(TAG, "WiFi attempt replaced; waiting for STA start");
        return true;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "STA config failed: %s", esp_err_to_name(ret));
        return false;
    }
    if (s_wifi_event_group) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
    atomic_store(&s_connect_requested,true);
    if(!atomic_load(&s_driver_started)){
        /* A previous replacement may have stopped the radio but failed to
         * restart it (e.g. NO_MEM). A fresh provisioning request retries it. */
        if (atomic_exchange(&s_restart_needed, false)) {
            ret = esp_wifi_start();
            if (ret != ESP_OK) {
                atomic_store(&s_restart_needed, true);
                atomic_store(&s_connect_requested, false);
                ESP_LOGE(TAG, "WiFi restart retry failed: %s", esp_err_to_name(ret));
                return false;
            }
        }
        ESP_LOGW(TAG,"WiFi credentials configured; waiting for STA start");
        return true;
    }

    /* 用驱动真实状态判断，event group bit 可能已被上面 clear */
    wifi_ap_record_t ap_info;
    bool driver_connected = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);

    if (wifi_sta_is_connected() || driver_connected) {
        /* 已连接: 先 disconnect，STA_DISCONNECTED 回调会用新配置 auto-reconnect */
        esp_wifi_disconnect();
        ESP_LOGW(TAG, "reconnecting to SSID=%s", ssid);
    } else {
        if (!wifi_connect_if_needed()) {
            return false;
        }
        ESP_LOGW(TAG, "connecting to SSID=%s", ssid);
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
    atomic_store(&s_connect_requested,false);
    if (s_wifi_event_group) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
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
