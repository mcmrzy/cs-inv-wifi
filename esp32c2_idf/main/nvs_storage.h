#ifndef NVS_STORAGE_H
#define NVS_STORAGE_H

#include "config.h"

typedef struct {
    uint32_t magic;
    uint32_t security_version;
    uint32_t partition_address;
    uint8_t source;
    char task_id[64];
    char version[32];
} StorageOtaPending;

bool storage_init();

bool storage_has_wifi_config();
bool storage_get_wifi(char* ssid, size_t ssid_len, char* pass, size_t pass_len);
bool storage_set_wifi(const char* ssid, const char* pass);
bool storage_clear_wifi();

bool storage_get_mqtt(char* host, size_t host_len, uint16_t* port);
bool storage_set_mqtt(const char* host, uint16_t port);

bool storage_has_sn();
bool storage_get_sn(char* sn, size_t sn_len);
bool storage_set_sn(const char* sn);
bool storage_get_key(char* key, size_t key_len);
bool storage_set_key(const char* key);

bool storage_get_ota_security_version_for_target(const char* target, uint32_t* version);
bool storage_set_ota_security_version_for_target(const char* target, uint32_t version);
bool storage_get_ota_pending(StorageOtaPending* pending);
bool storage_set_ota_pending(const StorageOtaPending* pending);
bool storage_clear_ota_pending(void);

/* Compatibility wrappers. New code must use the per-target APIs above. */
bool storage_get_ota_security_version(uint32_t* version);
bool storage_set_ota_security_version(uint32_t version);
bool storage_load_cmd_result(unsigned slot, char* task_id, size_t task_len,
                             char* response, size_t response_len);
bool storage_save_cmd_result(unsigned slot, const char* task_id, const char* response);
unsigned storage_get_cmd_next(void);
bool storage_set_cmd_next(unsigned next);

bool storage_has_ap_ssid();
bool storage_get_ap_ssid(char* ssid, size_t ssid_len);
bool storage_set_ap_ssid(const char* ssid);
bool storage_clear_ap_ssid();

void storage_factory_reset();

#endif
