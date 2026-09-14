#ifndef BLE_CT_PIN_H
#define BLE_CT_PIN_H

#include <stdbool.h>
#include <stdint.h>

// 校验 PIN
bool ble_ct_pin_verify(const char *sn, const char *pin, const uint8_t *peer_addr);

// 计算 PIN（供工厂工具或测试）
uint32_t ble_ct_compute_pin(const char *sn);

// 锁定剩余秒数
uint32_t ble_ct_pin_lockout_remaining(const uint8_t *peer_addr);

#endif
