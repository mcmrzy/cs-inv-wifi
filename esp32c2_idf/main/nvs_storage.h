#ifndef NVS_STORAGE_H
#define NVS_STORAGE_H

#include "config.h"

void storage_init();

bool storage_has_wifi_config();
bool storage_get_wifi(char* ssid, size_t ssid_len, char* pass, size_t pass_len);
bool storage_set_wifi(const char* ssid, const char* pass);
void storage_clear_wifi();

bool storage_get_mqtt(char* host, size_t host_len, uint16_t* port);
bool storage_set_mqtt(const char* host, uint16_t port);

bool storage_has_sn();
bool storage_get_sn(char* sn, size_t sn_len);
bool storage_set_sn(const char* sn);
bool storage_get_key(char* key, size_t key_len);
bool storage_set_key(const char* key);

bool storage_has_ap_ssid();
bool storage_get_ap_ssid(char* ssid, size_t ssid_len);
bool storage_set_ap_ssid(const char* ssid);
void storage_clear_ap_ssid();

void storage_factory_reset();

#endif
