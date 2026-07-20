#include "config.h"
#include "state_machine.h"
#include "nvs_storage.h"
#include "uart_protocol.h"
#include "led_status.h"
#include "ap_api_server.h"
#include "mqtt_handler.h"
#include "mqtt_ota_handler.h"
#include "ota_service.h"
#include "arm_ota.h"
#include "offline_cache.h"
#include "sn_encoder.h"
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
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include <string.h>

static const char *TAG = "app";

EventGroupHandle_t s_wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;

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
        check_button();
        led_tick();
        state_machine_run();
        offline_cache_process();
        ota_service_note_main_loop_alive();
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
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
    esp_wifi_init(&cfg);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

    if (!storage_init()) {
        ESP_LOGE(TAG, "Persistent storage unavailable; startup aborted");
        return;
    }
    uart_protocol_init();
    led_init();
    offline_cache_init();

    // 控制台在 UART1, 协议在 UART0, 物理隔离
    esp_log_level_set("*", ESP_LOG_WARN);

    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << PIN_BTN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);
    ESP_LOGI(TAG, "BTN=GPIO%d", PIN_BTN);

    uart_protocol_set_callback(on_uart_frame);
    ap_api_server_set_provision_callback(on_ap_provision_done);

    if (storage_has_sn()) {
        storage_get_sn(g_device_sn, sizeof(g_device_sn));
        storage_get_key(g_device_key, sizeof(g_device_key));

        if (sn_validate(g_device_sn)) {
            SNFields sf;
            sn_parse(g_device_sn, &sf);
            char readable[32];
            sn_printable(g_device_sn, readable, sizeof(readable));
            ESP_LOGI(TAG, "SN loaded (valid): %s", readable);
            ESP_LOGI(TAG, "  Mfr=%s Country=%s Cust=%s Date=%s Seq=%s",
                     sf.manufacturer, sf.country, sf.customer,
                     sf.date_code, sf.sequence);
        } else {
            ESP_LOGW(TAG, "SN loaded (INVALID format): %s", g_device_sn);
        }
    } else {
        ESP_LOGI(TAG, "No SN in NVS, waiting for ARM to provide SN via UART");
        // g_device_sn 保持为空, AP 名称将使用 MAC 地址
    }

    mqtt_init(on_mqtt_message);
    if (!ota_service_init()) {
        ESP_LOGE(TAG, "OTA service initialization failed");
        return;
    }
    mqtt_ota_init();

    // 先启动 BLE 配网（此时 WiFi 仅 init 未 start，避免 RF 状态冲突导致 BLE 控制器崩溃）
    ble_prov_set_callback(on_provision_done);
    ble_prov_start(g_device_sn, FIRMWARE_VERSION);

    // BLE 启动后再启动 WiFi
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_wifi_set_mode(WIFI_MODE_APSTA);

    if (storage_has_wifi_config()) {
        storage_get_wifi(g_wifi_ssid, sizeof(g_wifi_ssid), g_wifi_pass, sizeof(g_wifi_pass));
        ESP_LOGI(TAG, "Loaded WiFi from NVS: %s", g_wifi_ssid);

        wifi_config_t sta_cfg = {0};
        strncpy((char *)sta_cfg.sta.ssid, g_wifi_ssid, sizeof(sta_cfg.sta.ssid) - 1);
        strncpy((char *)sta_cfg.sta.password, g_wifi_pass, sizeof(sta_cfg.sta.password) - 1);
        esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);

        g_sys_state = STATE_STA_CONNECTING;
        led1_set(LED_FAST_BLINK);
    } else {
        ESP_LOGI(TAG, "No saved WiFi -> AP Provision mode");
        g_sys_state = STATE_AP_PROVISION;
        led1_set(LED_SLOW_BLINK);
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
        esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    }

    esp_wifi_start();
    if (storage_has_wifi_config()) {
        esp_wifi_connect();
    }
    ESP_LOGI(TAG, "WiFi started after BLE init");

    if (!ap_api_server_start()) {
        ESP_LOGE(TAG, "AP App API failed to start");
        return;
    }
    sntp_init_cst8();
    state_machine_init();

    ESP_LOGI(TAG, "Setup done");

    if (xTaskCreate(main_loop_task, "main_loop", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Main loop task creation failed");
        return;
    }
    if (!ota_service_start_boot_health_check()) {
        ESP_LOGE(TAG, "OTA health-check task creation failed");
        return;
    }
}
