#ifndef BLE_PROV_H
#define BLE_PROV_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BLE_PROV_IDLE,
    BLE_PROV_RUNNING,
    BLE_PROV_DONE,
    BLE_PROV_ERROR
} BleProvState;

typedef void (*BleProvCallback)(const char *ssid, const char *password);

/**
 * @brief 启动 BLE 配网
 * @param device_sn 设备 SN 号（显示在 BLE 广播中）
 * @param fw_version 固件版本号
 */
void ble_prov_start(const char *device_sn, const char *fw_version);

/**
 * @brief 停止 BLE 配网
 */
void ble_prov_stop(void);

/**
 * @brief 获取当前配网状态
 */
BleProvState ble_prov_get_state(void);

/**
 * @brief 设置配网成功回调
 */
void ble_prov_set_callback(BleProvCallback cb);

/**
 * @brief 获取 BLE 配网服务的 service name（用于手机 App 发现）
 */
const char *ble_prov_get_service_name(void);

#endif // BLE_PROV_H
