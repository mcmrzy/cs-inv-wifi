#pragma once
#include "ble_hs.h"
#define BLE_GAP_EVENT_CONNECT 1
#define BLE_GAP_EVENT_DISCONNECT 2
#define BLE_GAP_EVENT_SUBSCRIBE 3
#define BLE_GAP_EVENT_ENC_CHANGE 4
#define BLE_GAP_EVENT_MTU 5
#define BLE_GAP_EVENT_REPEAT_PAIRING 6
#define BLE_GAP_REPEAT_PAIRING_RETRY 1
#define BLE_ERR_REM_USER_CONN_TERM 19
struct ble_gap_event {
    int type;
    struct { int status; uint16_t conn_handle; } connect, enc_change;
    struct { struct { uint16_t conn_handle; } conn; } disconnect;
    struct { uint16_t attr_handle, conn_handle; uint8_t cur_notify; } subscribe;
    struct { uint16_t value; } mtu;
    struct { uint16_t conn_handle; } repeat_pairing;
};
int ble_gap_terminate(uint16_t conn, uint8_t reason);
