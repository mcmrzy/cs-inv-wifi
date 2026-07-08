#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include "config.h"

typedef enum {
    OTA_TARGET_ESP,
    OTA_TARGET_ARM
} OtaTarget;

typedef enum {
    OTA_STATUS_IDLE,
    OTA_STATUS_DOWNLOADING,
    OTA_STATUS_WRITING,
    OTA_STATUS_VERIFYING,
    OTA_STATUS_DONE,
    OTA_STATUS_ERROR
} OtaStatus;

typedef void (*OtaProgressCallback)(OtaTarget target, int percent, OtaStatus status, const char* message);

void ota_manager_init(void);

void ota_set_progress_callback(OtaProgressCallback cb);

bool ota_start_esp(const char* url);

bool ota_start_arm(const char* url);

OtaStatus ota_get_status(void);
OtaTarget ota_get_target(void);
int ota_get_progress(void);
const char* ota_get_message(void);

void ota_manager_process(void);

bool ota_is_busy(void);

const char* ota_get_esp_version(void);
const char* ota_get_arm_version(void);

#endif // OTA_MANAGER_H
