#include "ble_prov.h"
#include "wifi_provisioning/wifi_config.h"
#include "wifi_provisioning/scheme_ble.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "ble_prov";

static BleProvState s_state = BLE_PROV_IDLE;
static BleProvCallback s_callback = NULL;
static char s_service_name[32] = "CS_INV";

/* ---- 事件处理 ---- */

static void prov_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *data)
{
    switch (event_id) {
    case WIFI_PROV_START:
        ESP_LOGI(TAG, "BLE provisioning started");
        s_state = BLE_PROV_RUNNING;
        break;

    case WIFI_PROV_CRED_RECV: {
        wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)data;
        ESP_LOGI(TAG, "Received credentials: SSID=%s", (char *)wifi_sta_cfg->ssid);
        /* 不在这里调用回调，等 WIFI_PROV_END 再调用 */
        break;
    }

    case WIFI_PROV_CRED_SUCCESS: {
        ESP_LOGI(TAG, "WiFi credentials applied successfully");
        /* 凭据验证成功，通知上层 */
        wifi_config_t cfg;
        if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK) {
            if (s_callback) {
                s_callback((const char *)cfg.sta.ssid, (const char *)cfg.sta.password);
            }
        }
        /* BLE 持续广播，不停止 */
        break;
    }

    case WIFI_PROV_CRED_FAIL: {
        ESP_LOGW(TAG, "WiFi connection failed with received credentials");
        /* BLE 持续广播，不停止 */
        break;
    }

    case WIFI_PROV_END:
        ESP_LOGI(TAG, "BLE provisioning ended (but keep advertising)");
        /* BLE 持续广播，不停止配网服务 */
        break;

    default:
        break;
    }
}

/* ---- 公开 API ---- */

void ble_prov_set_callback(BleProvCallback cb)
{
    s_callback = cb;
}

BleProvState ble_prov_get_state(void)
{
    return s_state;
}

const char *ble_prov_get_service_name(void)
{
    return s_service_name;
}

void ble_prov_start(const char *device_sn, const char *fw_version)
{
    if (s_state == BLE_PROV_RUNNING) return;

    /* 构建 service name: CS_INV + SN后6位 */
    if (device_sn && strlen(device_sn) >= 6) {
        snprintf(s_service_name, sizeof(s_service_name), "CS_INV_%s",
                 device_sn + strlen(device_sn) - 6);
    } else {
        snprintf(s_service_name, sizeof(s_service_name), "CS_INV");
    }

    ESP_LOGI(TAG, "Starting BLE provisioning, service: %s", s_service_name);
    ESP_LOGI(TAG, "  SN: %s", device_sn ? device_sn : "N/A");
    ESP_LOGI(TAG, "  FW: %s", fw_version ? fw_version : "N/A");

    /* 初始化 provisioning manager */
    wifi_prov_mgr_config_t config = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = {
            .event_cb = NULL,
            .user_data = NULL
        }
    };

    esp_err_t ret = wifi_prov_mgr_init(config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wifi_prov_mgr_init failed: %s", esp_err_to_name(ret));
        s_state = BLE_PROV_ERROR;
        return;
    }

    /* 注册事件处理 */
    esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID,
                               &prov_event_handler, NULL);

    /* 自定义 BLE 设备名和服务数据 */
    wifi_prov_security_t security = WIFI_PROV_SECURITY_1;
    const char *pop = NULL;  /* 不使用 Proof of Possession */

    /* 启动配网，带重试 */
    ret = wifi_prov_mgr_start_provisioning(security, pop, s_service_name, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "BLE prov start failed (%s), retrying in 2s...",
                 esp_err_to_name(ret));
        wifi_prov_mgr_deinit();
        vTaskDelay(pdMS_TO_TICKS(2000));

        /* 重新初始化 */
        ret = wifi_prov_mgr_init(config);
        if (ret == ESP_OK) {
            esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID,
                                       &prov_event_handler, NULL);
            ret = wifi_prov_mgr_start_provisioning(security, pop,
                                                    s_service_name, NULL);
        }
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wifi_prov_mgr_start_provisioning failed: %s",
                 esp_err_to_name(ret));
        wifi_prov_mgr_deinit();
        s_state = BLE_PROV_ERROR;
        return;
    }

    s_state = BLE_PROV_RUNNING;
    ESP_LOGI(TAG, "BLE provisioning started, use 'ESP BLE Provisioning' app");
}

void ble_prov_stop(void)
{
    /* BLE 持续广播，不停止 */
    ESP_LOGI(TAG, "BLE provisioning stop requested (ignored, keep advertising)");
}
