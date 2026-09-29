#pragma once
#include "ble_hs.h"
#define BLE_GATT_ACCESS_OP_WRITE_CHR 2
#define BLE_GATT_ACCESS_OP_READ_CHR 1
#define BLE_GATT_SVC_TYPE_PRIMARY 1
#define BLE_GATT_CHR_F_WRITE 1
#define BLE_GATT_CHR_F_NOTIFY 2
#define BLE_GATT_CHR_F_READ 4
typedef struct { int type; } ble_uuid_t;
typedef struct { ble_uuid_t u; uint8_t value[16]; } ble_uuid128_t;
#define BLE_UUID128_INIT(...) { {0}, {__VA_ARGS__} }
struct ble_gatt_access_ctxt { uint8_t op; struct os_mbuf *om; };
struct ble_gatt_chr_def {
    const ble_uuid_t *uuid;
    int (*access_cb)(uint16_t, uint16_t, struct ble_gatt_access_ctxt *, void *);
    int flags;
    uint16_t *val_handle;
};
struct ble_gatt_svc_def { int type; const ble_uuid_t *uuid; struct ble_gatt_chr_def *characteristics; };
int ble_gatts_count_cfg(const struct ble_gatt_svc_def *svcs);
int ble_gatts_add_svcs(const struct ble_gatt_svc_def *svcs);
int ble_gattc_notify_custom(uint16_t conn, uint16_t handle, struct os_mbuf *om);
