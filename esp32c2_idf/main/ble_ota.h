#ifndef BLE_OTA_H
#define BLE_OTA_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BLE_OTA_IDLE = 0,
    BLE_OTA_READY,
    BLE_OTA_RECEIVING,
    BLE_OTA_VERIFYING,
    BLE_OTA_DONE,
    BLE_OTA_ERROR,
} BleOtaState;

typedef void (*BleOtaFinishCallback)(bool success, const char *message);

/**
 * @brief 注册 BLE OTA GATT 服务（在 ble_prov_start 之后调用）
 */
bool ble_ota_init(void);

/**
 * @brief BLE OTA 是否正在进行
 */
bool ble_ota_is_active(void);

/**
 * @brief 取消当前 OTA
 */
void ble_ota_cancel(void);

/**
 * @brief 设置 OTA 完成回调
 */
void ble_ota_set_finish_callback(BleOtaFinishCallback cb);

/**
 * @brief 通知 BLE OTA 连接状态变化（由 ble_prov 调用）
 * @param conn_handle 连接句柄，BLE_HS_CONN_HANDLE_NONE 表示断开
 */
void ble_ota_on_conn_change(uint16_t conn_handle);

#endif // BLE_OTA_H
