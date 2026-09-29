#pragma once
#include <stdint.h>

uint16_t ble_prov_get_ota_ctrl_handle(void);
uint16_t ble_prov_get_ota_data_handle(void);
uint16_t ble_prov_get_ota_status_handle(void);
void ble_prov_set_ota_access_cb(void *cb);
void *ble_prov_get_ota_access_cb(void);
void ble_prov_set_conn_callback(void (*cb)(uint16_t conn_handle));
