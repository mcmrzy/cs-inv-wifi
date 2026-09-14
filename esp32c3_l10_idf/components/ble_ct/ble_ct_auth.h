#ifndef BLE_CT_AUTH_H
#define BLE_CT_AUTH_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BLE_CT_AUTH_UNAUTHENTICATED,
    BLE_CT_AUTH_AUTHENTICATED,
} BleCtAuthState;

void ble_ct_auth_init(void);
bool ble_ct_auth_is_bound(void);
bool ble_ct_auth_is_authenticated(void);
void ble_ct_auth_reset_session(void);

// 绑定窗口
bool ble_ct_auth_is_bind_window_open(void);
void ble_ct_auth_open_bind_window(void);

// GATT 访问回调
struct ble_gatt_access_ctxt;
int ble_ct_auth_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg);

// AUTH notify
void ble_ct_auth_notify(const char *json, uint16_t len);

#endif // BLE_CT_AUTH_H
