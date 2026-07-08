/**
 * ================================================================
 *  MQTT OTA 处理模块 (ESP-IDF)
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
  char command[16];
  char target[8];
  char firmware_id[64];
  char url[256];
  char version[32];
  char md5[33];
  uint32_t file_size;
  uint16_t timeout;
  char task_id[64];
  uint32_t timestamp;
};

// ==================== API ====================

void mqtt_ota_init(void);
bool mqtt_ota_handle_command(const char* topic, const uint8_t* payload, unsigned int len);
void mqtt_ota_process(void);

void mqtt_ota_report_status(const char* target, const char* state, int progress,
                            const char* detail);
void mqtt_ota_report_error(const char* target, const char* error_code,
                           const char* error_message);

bool mqtt_ota_is_busy(void);
const char* mqtt_ota_get_task_id(void);

#endif // MQTT_OTA_HANDLER_H
