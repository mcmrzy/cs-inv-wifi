#ifndef OTA_SERVICE_H
#define OTA_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    OTA_SOURCE_MQTT = 0,
    OTA_SOURCE_LOCAL = 1,
} OtaSource;

typedef enum {
    OTA_TARGET_ESP = 0,
    OTA_TARGET_ARM = 1,
    OTA_TARGET_DSP = 2,
    OTA_TARGET_BMS = 3,
} OtaTarget;

typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_ACCEPTED,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_RECEIVING,
    OTA_STATE_VERIFYING,
    OTA_STATE_INSTALLING,
    OTA_STATE_CANCELLING,
    OTA_STATE_CANCELLED,
    OTA_STATE_REBOOTING,
    OTA_STATE_SUCCEEDED,
    OTA_STATE_FAILED,
    OTA_STATE_ROLLED_BACK,
} OtaState;

typedef enum {
    OTA_RESULT_OK = 0,
    OTA_RESULT_DUPLICATE,
    OTA_RESULT_INVALID_PARAMS,
    OTA_RESULT_INVALID_TARGET,
    OTA_RESULT_TLS_REQUIRED,
    OTA_RESULT_SIGNATURE_REQUIRED,
    OTA_RESULT_SIGNATURE_INVALID,
    OTA_RESULT_SECURITY_READ_FAILED,
    OTA_RESULT_ROLLBACK_REJECTED,
    OTA_RESULT_BUSY,
    OTA_RESULT_FILE_TOO_LARGE,
    OTA_RESULT_SIZE_MISMATCH,
    OTA_RESULT_HASH_MISMATCH,
    OTA_RESULT_HTTP_ERROR,
    OTA_RESULT_TIMEOUT,
    OTA_RESULT_READ_FAILED,
    OTA_RESULT_FLASH_WRITE_FAILED,
    OTA_RESULT_ARM_TRANSFER_FAILED,
    OTA_RESULT_PERSIST_FAILED,
    OTA_RESULT_NOT_CANCELLABLE,
    OTA_RESULT_TASK_CREATE_FAILED,
    OTA_RESULT_INTERNAL,
    OTA_RESULT_TARGET_UNSUPPORTED,
    OTA_RESULT_INSUFFICIENT_STORAGE,
} OtaResult;

typedef struct {
    OtaSource source;
    OtaTarget target;
    char task_id[64];
    char version[32];
    char url[256];
    char sha256[65];
    char signature[132];
    uint32_t security_version;
    uint32_t file_size;
    uint16_t timeout_seconds;
    uint32_t timestamp;
} OtaManifest;

typedef struct {
    char task_id[64];
    OtaSource source;
    OtaTarget target;
    OtaState state;
    uint8_t progress;
    uint32_t received;
    uint32_t total;
    char version[32];
    char error_code[32];
    /* 128: 失败 message 现在带堆余量 + https 提示(见 ota_service.c detail_append_*) */
    char message[128];
} OtaSnapshot;

/* Return bytes read, 0 for temporarily no data, or a negative value on error. */
typedef int (*OtaStreamReader)(void* context, uint8_t* buffer, size_t capacity);
typedef void (*OtaStatusObserver)(const OtaSnapshot* snapshot);

/* 状态订阅者上限: 现有 MQTT 上报 + BLE 本地两个适配器, 留出余量。
 * set_observer 是追加语义(不是替换), 重复注册同一函数会被忽略。 */
#define OTA_MAX_OBSERVERS 4

bool ota_service_init(void);
void ota_service_set_observer(OtaStatusObserver observer);
bool ota_service_start_boot_health_check(void);
void ota_service_note_main_loop_alive(void);

OtaResult ota_service_start_remote(const OtaManifest* manifest);
OtaResult ota_service_start_local(const OtaManifest* manifest,
                                  OtaStreamReader reader, void* reader_context);
OtaResult ota_service_cancel(const char* task_id);

bool ota_service_get_snapshot(OtaSnapshot* snapshot);
bool ota_service_is_busy(void);
const char* ota_service_get_task_id(void);
void ota_service_process(void);
bool ota_service_schedule_reboot(uint32_t delay_ms);

const char* ota_result_code(OtaResult result);
const char* ota_state_name(OtaState state);
const char* ota_target_name(OtaTarget target);
const char* ota_source_name(OtaSource source);

#endif
