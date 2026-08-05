#ifndef MQTT_HANDLER_H
#define MQTT_HANDLER_H

#include "config.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*MQTTMessageHandler)(const char *topic, const char *payload,
                                   unsigned int len);

/* 设备身份 (SN/Key, 由 app_main 从 NVS 加载) */
extern char g_device_sn[SN_MAX_LEN];
extern char g_device_key[65];

/* 初始化 (读取 SN/Key/Broker, 创建客户端), 返回是否成功 */
bool mqtt_init(MQTTMessageHandler msg_handler);

/* 启动/重连 (带退避), 供主循环周期调用 */
bool mqtt_connect(void);

void mqtt_disconnect(void);

/*
 * 发布 (自动拼接 cs_inv/{sn}/ 前缀 + 信封 {"t":..,"v":1,"data":<data>}):
 *  返回 0=已发送, 1=已入离线缓存, <0=失败
 */
int mqtt_publish(const char *sub_topic, const char *data, int len,
                 int qos, int retain);

int mqtt_subscribe(const char *sub_topic, int qos);

bool mqtt_is_connected(void);
const char *mqtt_get_client_id(void);

/* 触发离线缓存补传 (QoS1 + PUBACK 等待) */
void mqtt_flush_offline_cache(void);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_HANDLER_H */
