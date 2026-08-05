/*
 * cmd_handler.h — 云端命令处理
 *
 * 订阅主题: cs_inv/{sn}/cmd (QoS1, mqtt_handler 回调进入)
 * 命令格式: {"cmd":"set_charge_limit","value":20}  (兼容 name/action 键)
 * 响应主题: cs_inv/{sn}/cmd/response
 */
#ifndef CMD_HANDLER_H
#define CMD_HANDLER_H

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化 (由 app_main 调用) */
void cmd_handler_init(void);

/* MQTT 消息回调入口 (mqtt_handler 转发) */
void cmd_handler_on_mqtt_message(const char *topic, const char *payload,
                                 unsigned int len);

#ifdef __cplusplus
}
#endif

#endif /* CMD_HANDLER_H */
