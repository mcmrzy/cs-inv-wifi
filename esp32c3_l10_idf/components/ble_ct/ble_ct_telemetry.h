#ifndef BLE_CT_TELEMETRY_H
#define BLE_CT_TELEMETRY_H

#include <stdbool.h>
#include <stdint.h>

struct ble_gatt_access_ctxt;

/* 推送遥测（已鉴权 + 已订阅才发送；分帧 + 缓存快照） */
void ble_ct_telemetry_notify(const char *json, uint16_t len);
bool ble_ct_telemetry_should_push(void);

/* 更新最近一次遥测快照（推送时自动调用；也可由上层主动更新） */
void ble_ct_telemetry_set_snapshot(const char *json, uint16_t len);

/* TELEMETRY 特征 Read 回调（由 ble_ct 的 GATT access 转发），返回最近快照 */
int ble_ct_telemetry_read_cb(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg);

#endif
