#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_bt_main.h"

static const char *TAG = "ble_test";

// BLE广播数据
static esp_ble_adv_params_t adv_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x40,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .peer_addr = {0},
    .peer_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

// 广播数据
static uint8_t adv_data[] = {
    0x02, 0x01, 0x06,       // Flags
    0x0A, 0x09, 'E', 'S', 'P', '3', '2', 'C', '2', '_', 'B', 'T',  // Complete Local Name
};

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        ESP_LOGI(TAG, "BLE advertising data set, starting advertising...");
        esp_ble_gap_start_advertising(&adv_params);
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Advertising start failed");
        } else {
            ESP_LOGI(TAG, "Advertising started successfully!");
            ESP_LOGI(TAG, "Device name: ESP32C2_BT");
            ESP_LOGI(TAG, "Search for 'ESP32C2_BT' in Bluetooth settings");
        }
        break;
    default:
        break;
    }
}

void app_main(void)
{
    esp_err_t ret;

    // 初始化NVS
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " ESP32-C2 BLE Test");
    ESP_LOGI(TAG, "========================================");

    // 释放经典蓝牙内存
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    // 蓝牙控制器配置
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(TAG, "Bluetooth controller init failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(TAG, "Bluetooth controller enable failed: %s", esp_err_to_name(ret));
        return;
    }

    // 初始化蓝牙栈
    ret = esp_bluedroid_init();
    if (ret) {
        ESP_LOGE(TAG, "Bluetooth stack init failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(TAG, "Bluetooth stack enable failed: %s", esp_err_to_name(ret));
        return;
    }

    // 注册GAP回调
    ret = esp_ble_gap_register_callback(gap_event_handler);
    if (ret) {
        ESP_LOGE(TAG, "GAP register callback failed: %s", esp_err_to_name(ret));
        return;
    }

    // 设置广播数据
    ret = esp_ble_gap_config_adv_data_raw(adv_data, sizeof(adv_data));
    if (ret) {
        ESP_LOGE(TAG, "Config adv data failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "BLE initialization done, waiting for advertising...");
}
