/**
 * ================================================================
 *  MQTT OTA 处理模块
 *
 *  功能：
 *    - 接收 MQTT OTA 命令（start, stop, query）
 *    - HTTP 下载固件
 *    - 实时上报进度到 MQTT
 *    - 支持 ESP 和 ARM 固件升级
 * ================================================================
 */

#ifndef MQTT_OTA_HANDLER_H
#define MQTT_OTA_HANDLER_H

#include <stdint.h>
#include <stdbool.h>

// ==================== OTA 命令结构 ====================
struct MqttOtaCommand {
  char command[16];              // "start", "stop", "query"
  char target[8];                // "esp", "arm"
  char firmware_id[64];
  char url[256];                 // 固件下载 URL
  char version[32];
  char md5[33];                  // MD5 校验值（32字符 + null）
  uint32_t file_size;            // 文件大小
  uint16_t timeout;              // 超时（秒）
  char task_id[64];              // 任务 ID
  uint32_t timestamp;
};

// ==================== API ====================

// 初始化 MQTT OTA 处理（在 setup() 中调用）
void mqtt_ota_init();

// 处理 MQTT OTA 命令（在 on_mqtt_message 回调中调用）
// topic: 完整主题
// payload: JSON 格式的命令
// len: payload 长度
void mqtt_ota_handle_command(const char* topic, const uint8_t* payload, unsigned int len);

// 处理循环（在 main loop() 中调用）
void mqtt_ota_process();

// 上报 OTA 状态到 MQTT
void mqtt_ota_report_status(const char* target, const char* state, int progress,
                            const char* detail);

// 上报错误信息
void mqtt_ota_report_error(const char* target, const char* error_code,
                           const char* error_message);

// 检查是否正在进行 OTA
bool mqtt_ota_is_busy();

// 获取当前任务 ID
const char* mqtt_ota_get_task_id();

#endif // MQTT_OTA_HANDLER_H
