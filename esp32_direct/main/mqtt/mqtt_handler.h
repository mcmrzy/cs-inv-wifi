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

/* Main-loop-owned BLE/TLS memory admission; SDK auto-reconnect is disabled. */
void mqtt_set_connect_allowed(bool allowed);
bool mqtt_is_connecting(void);
/* Called even while Wi-Fi is offline to recover an SDK task's silent exit. */
void mqtt_poll_connection(void);
/* Main loop, under OTA administrative guard. False means a cloud owner is
 * still draining; do not start BLE until the full client has been destroyed. */
bool mqtt_pause_for_local(void);
bool mqtt_resume_after_local(void);
bool mqtt_is_paused_for_local(void);

void mqtt_disconnect(void);

/*
 * 发布 (自动拼接 cs_inv/{sn}/ 前缀 + 信封 {"t":..,"v":1,"data":<data>}):
 *  返回 0=已发送, 1=已入离线缓存, <0=失败
 */
void mqtt_clear_retained_status(const char *sn);

int mqtt_publish(const char *sub_topic, const char *data, int len,
                 int qos, int retain);

/*
 * 实时发布: 与 mqtt_publish 同格式, 但断连时直接丢弃 (不写离线缓存)。
 * 用于调试日志之类"补传无价值、也不该挤占遥测补传槽位"的上行。
 * 返回 0=已发送, <0=失败/未连接
 */
int mqtt_publish_volatile(const char *sub_topic, const char *data, int len,
                          int qos);

/*
 * heartbeat 专用发布 (信封版本 3, 不走离线缓存):
 * data 为 ARM RunParamDef 原样转发的 {"run":[87 个 u16]}, 服务端按结构体
 * 定义解析。不缓存见 mqtt_handler.c 实现处注释 (缓存 payload 上限 512B /
 * 补传按 v2 重组信封会与 v3 冲突 / 状态快照补传价值低)。
 * 返回 0=已发送, <0=失败/未连接
 */
int mqtt_publish_heartbeat_raw(const char *sub_topic, const char *data,
                               int len);

int mqtt_subscribe(const char *sub_topic, int qos);

bool mqtt_is_connected(void);
const char *mqtt_get_client_id(void);

/* 触发离线缓存补传 (QoS1 + PUBACK 等待) */
void mqtt_flush_offline_cache(void);
/* Pause new cache workers; an existing worker completes its current PUBACK
 * only. False means still draining. Does not pause MQTT control/status/TLS. */
bool mqtt_set_cache_flush_paused(bool paused);

/*
 * OTA 过程留痕: 发布到 cs_inv/{sn}/ota/debug, 供人在 MQTT 侧远程判读
 * (等价于串口的 OTA 日志)。与 ota/status 分开, 不参与后端状态机。
 * detail 会做 JSON 转义与长度截断。
 * 仅实时发送，断连时丢弃；信封按实际长度临时分配，不占用 2KB 任务栈。
 */
void mqtt_publish_ota_trace(const char *stage, const char *detail);

/* 断线超过 timeout_ms 仍未重连 → true (供主循环强制重启客户端) */
bool mqtt_is_stuck(int64_t timeout_ms);

/* 销毁并重建 MQTT 客户端 (清除卡住的内部重连状态) */
void mqtt_force_restart(void);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_HANDLER_H */
