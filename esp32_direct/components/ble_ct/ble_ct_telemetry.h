#ifndef BLE_CT_TELEMETRY_H
#define BLE_CT_TELEMETRY_H

#include <stdbool.h>
#include <stdint.h>

struct ble_gatt_access_ctxt;

/* Invalidate only the in-progress long read; retain latest telemetry. */
void ble_ct_telemetry_reset_connection(void);

/* Always refresh the snapshot; notify only when connected and subscribed.
 * Uses negotiated ATT MTU. More than eight frames requires framing_v1. */
void ble_ct_telemetry_notify(const char *json, uint16_t len);
bool ble_ct_telemetry_should_push(void);

/* Atomically update the snapshot. Empty or >1024-byte input is rejected intact. */
void ble_ct_telemetry_set_snapshot(const char *json, uint16_t len);

/* TELEMETRY 特征 Read 回调（由 ble_ct 的 GATT access 转发），返回最近快照 */
int ble_ct_telemetry_read_cb(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg);

#endif
