#ifndef OTA_RESULT_H
#define OTA_RESULT_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    char status[16];   // "done" / "error"
    char version[32];  // 新版本号
    char message[64];  // 描述
    char task_id[64];
    char target[8];
} OtaResult;

// 保存OTA结果到NVS（重启前调用）
void ota_result_save(const char* status, const char* version, const char* message);
void ota_task_result_save(const char* task_id, const char* target,
                          const char* status, const char* version, const char* message);

// 从NVS加载OTA结果（成功返回true）
bool ota_result_load(OtaResult* result);

// 清除NVS中的OTA结果
void ota_result_clear();
uint32_t ota_security_version_get();
bool ota_security_version_advance(uint32_t version);

#endif
