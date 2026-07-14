#ifndef MQTT_HANDLER_H
#define MQTT_HANDLER_H

#include "config.h"
#include "sn_encoder.h"
#include <stdint.h>
#include <stdbool.h>

// MQTT 消息回调函数类型
// topic: 完整主题名
// payload: 消息内容（已确保 null-terminated）
// len: payload 长度
typedef void (*MQTTMessageHandler)(const char* topic, const char* payload, unsigned int len);

// ==================== 初始化与连接 ====================

// 初始化 MQTT 客户端（从 NVS 读取 SN、Broker 配置）
// msg_handler: 收到消息时的回调函数
// 返回: true=成功, false=失败（如 SN 未设置或无效）
bool mqtt_init(MQTTMessageHandler msg_handler);

// 尝试连接 MQTT Broker（阻塞，带重试间隔控制）
// 返回: true=已连接, false=连接中或失败
bool mqtt_connect(void);

// 尝试连接 MQTT Broker（可强制跳过重试间隔）
// force: true=跳过重连间隔，立即尝试连接
// 返回: true=已连接, false=连接中或失败
bool mqtt_connect_ex(bool force);

// 断开 MQTT 连接
void mqtt_disconnect(void);

// 轮询处理（必须在 loop() 中频繁调用）
void mqtt_loop(void);

// ==================== 发布与订阅 ====================

// 向指定子主题发布消息
// sub_topic: 子主题（如 "telemetry"，完整主题为 cs_inv/{SN}/telemetry）
// data: 消息内容
// len: 消息长度（字节）
// qos: QoS 等级（0/1/2）
// retain: 是否保留消息
// 返回: 0=成功, 负数=错误码
int mqtt_publish(const char* sub_topic, const char* data, int len, int qos, int retain);

// 向指定子主题发布信封格式消息
// 自动包装为 {"t":<NTP秒>,"data":<arm_payload>} 格式
// topic: 子主题
// arm_payload: ARM 原始 payload（JSON 对象）
// payload_len: payload 长度
// retain: 是否保留消息
// 返回: true=成功, false=失败
bool mqtt_publish_envelope(const char *topic, const char *arm_payload, unsigned int payload_len, bool retain = false);

// 订阅指定子主题
// sub_topic: 子主题（如 "cmd"，完整主题为 cs_inv/{SN}/cmd）
// qos: QoS 等级
// 返回: 0=成功, 负数=错误码
int mqtt_subscribe(const char* sub_topic, int qos);

// ==================== 状态查询 ====================

// 检查是否已连接
bool mqtt_is_connected(void);

// 获取 Client ID（即 SN 号）
const char* mqtt_get_client_id(void);

// 获取状态主题（用于 LWT）
const char* mqtt_get_status_topic(void);

// 获取 LWT 离线消息内容
const char* mqtt_get_lwt_payload(void);

// ==================== 配置 ====================

// 动态设置 MQTT Broker（会保存到 NVS）
bool mqtt_set_broker(const char* host, uint16_t port);

#endif // MQTT_HANDLER_H
