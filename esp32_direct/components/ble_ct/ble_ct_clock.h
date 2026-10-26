#ifndef BLE_CT_CLOCK_H
#define BLE_CT_CLOCK_H

#include <stdint.h>

uint32_t ble_ct_now(void);
void ble_ct_calibrate(uint32_t unix_ts);
void ble_ct_clock_init(void);

#endif
