#ifndef NVS_STORAGE_H
#define NVS_STORAGE_H

#include "config.h"

// ==================== 初始化存储 ====================
void storage_init();

// ==================== WiFi 凭据 ====================
bool storage_has_wifi_config();
bool storage_get_wifi(char* ssid, size_t ssid_len, char* pass, size_t pass_len);
bool storage_set_wifi(const char* ssid, const char* pass);
void storage_clear_wifi();

// ==================== MQTT 配置 ====================
bool storage_get_mqtt(char* host, size_t host_len, uint16_t* port);
bool storage_set_mqtt(const char* host, uint16_t port);

// ==================== 设备信息 ====================
bool storage_has_sn();
bool storage_get_sn(char* sn, size_t sn_len);
bool storage_set_sn(const char* sn);
bool storage_get_key(char* key, size_t key_len);
bool storage_set_key(const char* key);

// ==================== AP 热点名称 ====================
bool storage_has_ap_ssid();
bool storage_get_ap_ssid(char* ssid, size_t ssid_len);
bool storage_set_ap_ssid(const char* ssid);
void storage_clear_ap_ssid();

// ==================== 恢复出厂 ====================
void storage_factory_reset();

// ==================== 加密工具 ====================
void xor_crypt(const char* key, const uint8_t* input, size_t len, uint8_t* output);

#endif
