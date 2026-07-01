#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include "config.h"

// ==================== OTA 目标 ====================
enum OtaTarget {
  OTA_TARGET_ESP,
  OTA_TARGET_ARM
};

// ==================== OTA 状态 ====================
enum OtaStatus {
  OTA_STATUS_IDLE,
  OTA_STATUS_DOWNLOADING,
  OTA_STATUS_WRITING,
  OTA_STATUS_VERIFYING,
  OTA_STATUS_DONE,
  OTA_STATUS_ERROR
};

// ==================== OTA 进度回调 ====================
typedef void (*OtaProgressCallback)(OtaTarget target, int percent, OtaStatus status, const char* message);

// ==================== API ====================

// 初始化 OTA 管理器
void ota_manager_init();

// 设置进度回调
void ota_set_progress_callback(OtaProgressCallback cb);

// 开始 ESP 自身 OTA（从 URL 下载固件）
// 返回 true 表示已开始（异步执行），false 表示参数错误或正在升级
bool ota_start_esp(const char* url);

// 开始 ARM OTA（从 URL 下载固件，然后通过串口发送给 ARM）
bool ota_start_arm(const char* url);

// 获取当前 OTA 状态
OtaStatus ota_get_status();
OtaTarget ota_get_target();
int ota_get_progress();
const char* ota_get_message();

// OTA 处理循环（在 main loop 中调用）
void ota_manager_process();

// 检查是否正在 OTA
bool ota_is_busy();

// 外部设置 OTA 状态和进度（用于 HTTP 本地 OTA）
void ota_set_status(OtaStatus status, int progress, const char* message);

// 获取设备固件信息
const char* ota_get_esp_version();
const char* ota_get_arm_version();

#endif // OTA_MANAGER_H
