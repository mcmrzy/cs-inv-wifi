#include "config.h"
#include "state_machine.h"
#include "nvs_storage.h"
#include "uart_protocol.h"
#include "ap_api_server.h"
#include "mqtt_handler.h"
#include "mqtt_ota_handler.h"
#include "ota_service.h"
#include "arm_ota.h"
#include "offline_cache.h"
#include "ble_prov.h"

#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_sntp.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include <string.h>

static const char *TAG = "app";

EventGroupHandle_t s_wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;

static void log_heap(const char *stage)
{
    ESP_LOGW(TAG, "Heap %-16s free=%lu largest=%lu min=%lu", stage,
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned long)esp_get_minimum_free_heap_size());
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *data)
{
    (void)arg;
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "WIFI_EVENT: STA connected");
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *evt = data;
            ESP_LOGI(TAG, "WIFI_EVENT: STA disconnected, reason=%d", evt->reason);
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            break;
        }
        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "WIFI_EVENT: AP started");
            break;
        case WIFI_EVENT_AP_STOP:
            ESP_LOGI(TAG, "WIFI_EVENT: AP stopped");
            break;
        case WIFI_EVENT_AP_STACONNECTED: {
            const wifi_event_ap_staconnected_t *evt = data;
            ESP_LOGI(TAG, "WIFI_EVENT: AP station connected, MAC=" MACSTR, MAC2STR(evt->mac));
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            const wifi_event_ap_stadisconnected_t *evt = data;
            ESP_LOGI(TAG, "WIFI_EVENT: AP station disconnected, MAC=" MACSTR, MAC2STR(evt->mac));
            break;
        }
        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *evt = data;
        ESP_LOGI(TAG, "WIFI_EVENT: Got IP: " IPSTR, IP2STR(&evt->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void sntp_init_cst8(void)
{
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_init();
    setenv("TZ", "CST-8", 1);
    tzset();
    ESP_LOGI(TAG, "SNTP initialized (CST-8)");
}

static void main_loop_task(void *arg)
{
    ESP_LOGI(TAG, "Main loop task started");

    esp_task_wdt_add(xTaskGetCurrentTaskHandle());

    for (;;) {
        uart_protocol_process();
        state_machine_run();
        offline_cache_process();
        ota_service_note_main_loop_alive();
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

static void on_ble_provision_done(const char *ssid, const char *password)
{
    ESP_LOGI(TAG, "BLE provision received: SSID=%s", ssid);

    storage_set_wifi(ssid, password);

    esp_restart();
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " INVERTER WiFi MODULE (ESP-IDF)");
    ESP_LOGI(TAG, " ESP32-C2 + Custom UART + MQTT");
    ESP_LOGI(TAG, "========================================");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS: erase and reinit");
        nvs_flash_erase();
        nvs_flash_init();
    }

    s_wifi_event_group = xEventGroupCreate();

    esp_netif_init();
    esp_event_loop_create_default();

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(ret));
        return;
    }

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

    if (!storage_init()) {
        ESP_LOGE(TAG, "Persistent storage unavailable; startup aborted");
        return;
    }
    uart_protocol_init();
    offline_cache_init();

    // 控制台在 UART1, 协议在 UART0, 物理隔离
    esp_log_level_set("*", ESP_LOG_WARN);

    uart_protocol_set_callback(on_uart_frame);
    ap_api_server_set_provision_callback(on_ap_provision_done);

    if (storage_has_sn()) {
        storage_get_sn(g_device_sn, sizeof(g_device_sn));
        storage_get_key(g_device_key, sizeof(g_device_key));
        ESP_LOGI(TAG, "SN loaded: %s", g_device_sn);
    } else {
        ESP_LOGI(TAG, "No SN in NVS, waiting for ARM to provide SN via UART");
    }

    if (storage_has_sn()) mqtt_init(on_mqtt_message);
    if (!ota_service_init()) {
        ESP_LOGE(TAG, "OTA service initialization failed");
        return;
    }
    mqtt_ota_init();

    const bool has_wifi_config = storage_has_wifi_config();
    log_heap("before APSTA");
    ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi APSTA mode failed: %s", esp_err_to_name(ret));
        return;
    }

    if (has_wifi_config) {
        storage_get_wifi(g_wifi_ssid, sizeof(g_wifi_ssid), g_wifi_pass, sizeof(g_wifi_pass));
        ESP_LOGI(TAG, "Loaded WiFi from NVS: %s", g_wifi_ssid);

        wifi_config_t sta_cfg = {0};
        strncpy((char *)sta_cfg.sta.ssid, g_wifi_ssid, sizeof(sta_cfg.sta.ssid) - 1);
        strncpy((char *)sta_cfg.sta.password, g_wifi_pass, sizeof(sta_cfg.sta.password) - 1);
        ret = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "STA configuration failed: %s", esp_err_to_name(ret));
            return;
        }

        g_sys_state = STATE_STA_CONNECTING;
    } else {
        ESP_LOGI(TAG, "No saved WiFi -> AP Provision mode");
        g_sys_state = STATE_AP_PROVISION;
    }

    {
        char ap_ssid[32];
        build_ap_ssid(ap_ssid, sizeof(ap_ssid));
        ESP_LOGI(TAG, "AP SSID: '%s'", ap_ssid);
        wifi_config_t ap_cfg = {0};
        strncpy((char *)ap_cfg.ap.ssid, ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
        ap_cfg.ap.ssid_len = strlen(ap_ssid);
        ap_cfg.ap.channel = 1;
        ap_cfg.ap.max_connection = 4;
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
        ret = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "AP configuration failed: %s", esp_err_to_name(ret));
            return;
        }
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi start failed: %s", esp_err_to_name(ret));
        return;
    }
    if (has_wifi_config) {
        ret = esp_wifi_connect();
        if (ret != ESP_OK) ESP_LOGW(TAG, "Initial STA connect failed: %s", esp_err_to_name(ret));
    }
    log_heap("after WiFi");

    // 启动BLE广播（无论有没有SN都启动，SN为空时使用MAC地址）
    ble_prov_set_callback(on_ble_provision_done);
    if (ble_prov_start(g_device_sn, FIRMWARE_VERSION)) {
        ESP_LOGW(TAG, "BLE advertising started: %s", ble_prov_get_service_name());
    } else {
        ESP_LOGE(TAG, "BLE advertising start failed");
    }

    if (!ap_api_server_start()) {
        ESP_LOGE(TAG, "AP App API failed to start");
        return;
    }
    log_heap("after App API");
    sntp_init_cst8();
    state_machine_init();

    if (xTaskCreate(main_loop_task, "main_loop", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Main loop task creation failed");
        return;
    }
    if (!ota_service_start_boot_health_check()) {
        ESP_LOGE(TAG, "OTA health-check task creation failed");
        return;
    }

    ESP_LOGI(TAG, "Setup done");
}
