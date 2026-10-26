#ifndef NVS_STORAGE_H
#define NVS_STORAGE_H

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* OTA 待确认记录 (重启后回滚检测用) */
typedef struct {
    uint32_t magic;
    uint32_t security_version;
    uint32_t partition_address;
    uint8_t source;
    char task_id[64];
    char version[32];
} StorageOtaPending;

bool storage_init(void);

bool storage_has_wifi_config(void);
bool storage_get_wifi(char *ssid, size_t ssid_len, char *pass, size_t pass_len);
bool storage_set_wifi(const char *ssid, const char *pass);
bool storage_clear_wifi(void);

bool storage_get_mqtt(char *host, size_t host_len, uint16_t *port);
bool storage_set_mqtt(const char *host, uint16_t port);

bool storage_has_sn(void);
bool storage_get_sn(char *sn, size_t sn_len);
bool storage_set_sn(const char *sn);
bool storage_get_key(char *key, size_t key_len);
bool storage_set_key(const char *key);

/* ---- OTA 安全版本 (per target: esp/arm) ---- */
bool storage_get_ota_security_version_for_target(const char *target, uint32_t *version);
bool storage_set_ota_security_version_for_target(const char *target, uint32_t version);
/* Claim one direct OTA task BEFORE scheduling any target work. Last 8 task IDs
 * per DSP/BMS survive restart, including failed/interrupted attempts. A retry
 * must use a new explicitly issued task ID. Caller serializes with OTA lock.
 * false: storage/record failure (fail closed); true+duplicate: do not execute. */
bool storage_claim_direct_ota_task(const char *target,const char *task_id,bool *duplicate);

/* ---- OTA 待确认记录 ---- */
bool storage_get_ota_pending(StorageOtaPending *pending);
bool storage_set_ota_pending(const StorageOtaPending *pending);
bool storage_clear_ota_pending(void);

void storage_factory_reset(void);

#ifdef __cplusplus
}
#endif

#endif
