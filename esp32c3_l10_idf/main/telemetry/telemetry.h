/*
 * telemetry.h — 遥测上报 (运行参数 → MQTT)
 *
 * V2.0 协议 (CS-L10-6K2_MQTT_上报协议设计_V2.0.md):
 *  - heartbeat  180s + 启动立即 + query_telemetry  49 位置数组 (v=2 信封)
 *  - info       连接时/信息变化/query_info        设备能力与固件信息
 *  - alarm      变化时 QoS1                        {source, code, level, state}
 *  - status/LWT 由 mqtt_handler 负责 (上线一次 + 遗嘱, 本模块不参与)
 */
#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化: 清零告警历史与时间戳 */
void telemetry_init(void);

/* 主循环周期调用 (每 100ms): 内部 180s 心跳调度 + 告警变化检测 */
void telemetry_tick(void);

/* MQTT 连接建立后调用: 立即上报 info + heartbeat + 请求 ARM 重读 info */
void telemetry_on_mqtt_connected(void);

/* 立即补报一次 heartbeat (query_telemetry 命令, 重置 180s 周期) */
void telemetry_request_heartbeat(void);

/* 立即补报一次 info (query_info 命令, 并请求 ARM 重读只读数据) */
void telemetry_request_info(void);

#ifdef __cplusplus
}
#endif

#endif /* TELEMETRY_H */
