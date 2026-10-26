#ifndef BLE_CT_H
#define BLE_CT_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BLE_CT_IDLE,
    BLE_CT_RUNNING,
    BLE_CT_ERROR
} BleCtState;

bool ble_ct_start(const char *device_sn, const char *fw_version);
void ble_ct_stop(void);
BleCtState ble_ct_get_state(void);

uint16_t ble_ct_get_conn_handle(void);
const char *ble_ct_get_sn(void);
const char *ble_ct_get_session_id(void);
/* Negotiated per connection by open_session; resets on every host/connection reset. */
bool ble_ct_framing_enabled(void);
void ble_ct_set_framing_enabled(bool enabled);

// 句柄导出
uint16_t ble_ct_get_telemetry_handle(void);
uint16_t ble_ct_get_result_handle(void);

// 订阅状态
bool ble_ct_is_telemetry_subscribed(void);
bool ble_ct_is_result_subscribed(void);

// GAP 事件处理（由 ble_prov 调用）
struct ble_gap_event;
int ble_ct_on_gap_event(struct ble_gap_event *event, void *arg);

// 注册 GATT 服务（由 ble_prov pre-host-start 回调调用）
void ble_ct_register_services(void);
/* Reset volatile GATT state when its owning NimBLE host is recreated. */
void ble_ct_reset_host(void);

#endif // BLE_CT_H
