/*
 * ota_mqtt_adapter.h — MQTT OTA 命令适配器
 *
 * 订阅主题: cs_inv/{sn}/ota/cmd (由 mqtt_handler 订阅并分发)
 * 命令格式: {"v":1,"action":"start|cancel|query","target":"esp|arm",
 *            "task_id":..,"url":..,"version":..,"sha256":..,
 *            "signature":..,"security_version":..,"size":..,"t":<unix>}
 * 状态上报: cs_inv/{sn}/ota/status
 *
 * 从 esp32c3_idf mqtt_handler.c 提取, 不引入原项目无关逻辑。
 */
#ifndef OTA_MQTT_ADAPTER_H
#define OTA_MQTT_ADAPTER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化 (注册 ota_service observer), 由 app_main 调用 */
void mqtt_ota_init(void);

/* 处理 ota/cmd 主题消息 (mqtt_handler 分发), 返回 true=识别为 OTA 命令 */
bool mqtt_ota_handle_command(const char *topic, const uint8_t *payload,
                             unsigned int length);

/* 周期处理 (兼容接口) */
void mqtt_ota_process(void);

bool mqtt_ota_is_busy(void);
const char *mqtt_ota_get_task_id(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_MQTT_ADAPTER_H */
