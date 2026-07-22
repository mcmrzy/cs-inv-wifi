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
static bool s_manager_initialized;
static bool s_handler_registered;
static bool s_credentials_ready;
static char s_pending_ssid[33];
static char s_pending_password[65];

static void prov_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *data);

static void release_manager(void)
{
    if (s_manager_initialized) {
        wifi_prov_mgr_deinit();
        s_manager_initialized = false;
    }
    if (s_handler_registered) {
        esp_event_handler_unregister(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID,
                                     &prov_event_handler);
        s_handler_registered = false;
    }
}

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
        wifi_config_t cfg = {0};
        if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK) {
            strlcpy(s_pending_ssid, (const char *)cfg.sta.ssid,
                    sizeof(s_pending_ssid));
            strlcpy(s_pending_password, (const char *)cfg.sta.password,
                    sizeof(s_pending_password));
            s_credentials_ready = true;
        }
        break;
    }

    case WIFI_PROV_CRED_FAIL: {
        ESP_LOGW(TAG, "WiFi connection failed with received credentials");
        /* BLE 持续广播，不停止 */
        break;
    }

    case WIFI_PROV_END:
        ESP_LOGI(TAG, "BLE provisioning ended; releasing provisioning memory");
        wifi_prov_mgr_deinit();
        s_manager_initialized = false;
        s_state = BLE_PROV_DONE;
        if (s_handler_registered) {
            esp_event_handler_unregister(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID,
                                         &prov_event_handler);
            s_handler_registered = false;
        }
        if (s_credentials_ready && s_callback) {
            s_credentials_ready = false;
            s_callback(s_pending_ssid, s_pending_password);
        }
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

bool ble_prov_start(const char *device_sn, const char *fw_version)
{
    if (s_state == BLE_PROV_RUNNING) return true;
    s_credentials_ready = false;
    s_pending_ssid[0] = '\0';
    s_pending_password[0] = '\0';

    /* 构建 service name: CS_INV_ + 完整SN */
    if (device_sn && strlen(device_sn) > 0) {
        snprintf(s_service_name, sizeof(s_service_name), "CS_INV_%s", device_sn);
    } else {
        snprintf(s_service_name, sizeof(s_service_name), "CS_INV");
    }

    ESP_LOGI(TAG, "Starting BLE provisioning, service: %s", s_service_name);
    ESP_LOGI(TAG, "  SN: %s", device_sn ? device_sn : "N/A");
    ESP_LOGI(TAG, "  FW: %s", fw_version ? fw_version : "N/A");

    /* 初始化 provisioning manager */
    wifi_prov_mgr_config_t config = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
    };

    esp_err_t ret = wifi_prov_mgr_init(config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wifi_prov_mgr_init failed: %s", esp_err_to_name(ret));
        s_state = BLE_PROV_ERROR;
        return false;
    }
    s_manager_initialized = true;

    /* 注册事件处理 */
    ret = esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID,
                                     &prov_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "event handler registration failed: %s", esp_err_to_name(ret));
        release_manager();
        s_state = BLE_PROV_ERROR;
        return false;
    }
    s_handler_registered = true;

    /* 自定义 BLE 设备名和服务数据 */
    wifi_prov_security_t security = WIFI_PROV_SECURITY_1;
    const char *pop = NULL;  /* 不使用 Proof of Possession */

    /* FREE_BTDM 释放后本次启动不能重新初始化，因此失败时交回 AP 模式。 */
    ret = wifi_prov_mgr_start_provisioning(security, pop, s_service_name, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wifi_prov_mgr_start_provisioning failed: %s",
                 esp_err_to_name(ret));
        release_manager();
        s_state = BLE_PROV_ERROR;
        return false;
    }

    s_state = BLE_PROV_RUNNING;
    ESP_LOGI(TAG, "BLE provisioning started, use 'ESP BLE Provisioning' app");
    return true;
}

void ble_prov_stop(void)
{
    if (!s_manager_initialized) return;
    ESP_LOGI(TAG, "Stopping BLE provisioning");
    wifi_prov_mgr_stop_provisioning();
}
