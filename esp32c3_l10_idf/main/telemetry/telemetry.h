/*
 * telemetry.h — 遥测上报 (运行参数 → MQTT)
 *
 * V2.1 协议 (CS-L10-6K2_MQTT_上报协议设计_V2.1.md):
 *  - heartbeat  30s + 启动立即 + query_telemetry  57 值数组 (v=2 信封)
 *  - info       连接时/信息变化/query_info        设备能力与固件信息 (17 字段)
 *  - config     连接时/写入生效后/query_config    42 键工程单位 + rev
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

/* 主循环周期调用 (每 100ms): 内部 30s 心跳调度 + 告警变化检测 */
void telemetry_tick(void);

/* MQTT 连接建立后调用: 立即上报 info + heartbeat + 请求 ARM 重读 info */
void telemetry_on_mqtt_connected(void);

/* 立即补报一次 heartbeat (query_telemetry 命令, 重置 30s 周期);
   仅写 volatile bool, 可从任意任务安全调用 */
void telemetry_request_heartbeat(void);

/* 立即补报一次 info (query_info 命令, 并请求 ARM 重读只读数据) */
void telemetry_request_info(void);

/* 请求重读控制参数并补报一次 config (query_config/写入生效/连接成功) */
void telemetry_request_config(void);

/* 重置心跳计时器: 任何有效上行数据发布后调用, 避免冗余心跳 */
void telemetry_reset_heartbeat_timer(void);

#ifdef __cplusplus
}
#endif

#endif /* TELEMETRY_H */
