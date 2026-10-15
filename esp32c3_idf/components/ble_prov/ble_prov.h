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
typedef void (*BleProvConnCallback)(uint16_t conn_handle);

/**
 * @brief 启动 BLE 配网
 * @param device_sn 设备 SN 号（显示在 BLE 广播中）
 * @param fw_version 固件版本号
 */
bool ble_prov_start(const char *device_sn, const char *fw_version);

/**
 * @brief 停止 BLE 广播（暂停，保留协议栈已初始化，可用 resume 快速恢复）
 */
void ble_prov_stop(void);

/**
 * @brief 恢复 BLE 广播（不重新初始化协议栈；ble_prov_start 之后可反复调用）
 */
void ble_prov_resume(void);

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

/**
 * @brief 设置 BLE 连接/断连事件回调
 */
void ble_prov_set_conn_callback(BleProvConnCallback cb);

/**
 * @brief 获取 OTA 特征的 val handle
 */
uint16_t ble_prov_get_ota_ctrl_handle(void);
uint16_t ble_prov_get_ota_data_handle(void);
uint16_t ble_prov_get_ota_status_handle(void);

/**
 * @brief 设置 OTA GATT 访问回调（由 ble_ota 调用）
 */
void ble_prov_set_ota_access_cb(void *cb);

#endif // BLE_PROV_H
