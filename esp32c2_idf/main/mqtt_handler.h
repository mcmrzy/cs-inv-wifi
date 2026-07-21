#ifndef MQTT_HANDLER_H
#define MQTT_HANDLER_H

#include "config.h"
#include <stdint.h>
#include <stdbool.h>

typedef void (*MQTTMessageHandler)(const char* topic, const char* payload, unsigned int len);

bool mqtt_init(MQTTMessageHandler msg_handler);
bool mqtt_connect(void);
void mqtt_on_network_lost(void);
bool mqtt_reconfigure(void);
void mqtt_disconnect(void);

int mqtt_publish(const char* sub_topic, const char* data, int len, int qos, int retain);
int mqtt_subscribe(const char* sub_topic, int qos);

bool mqtt_is_connected(void);
const char* mqtt_get_client_id(void);
const char* mqtt_get_status_topic(void);
const char* mqtt_get_lwt_payload(void);

bool mqtt_set_broker(const char* host, uint16_t port);

// 离线缓存刷写（重连后优先发送缓存数据）
void mqtt_flush_offline_cache(void);

#endif // MQTT_HANDLER_H
