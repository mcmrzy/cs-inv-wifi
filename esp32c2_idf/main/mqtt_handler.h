#ifndef MQTT_HANDLER_H
#define MQTT_HANDLER_H

#include "config.h"
#include "ota_service.h"
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

void mqtt_flush_offline_cache(void);

/* OTA command handler (merged from mqtt_ota_adapter.c) */
struct MqttOtaCommand {
  uint16_t protocol_version;
  char action[16];
  char target[8];
  char firmware_id[64];
  char url[256];
  char version[32];
  char sha256[65];
  char signature[132];
  uint32_t security_version;
  uint32_t file_size;
  uint16_t timeout;
  char task_id[64];
  char ota_task_id[64];
  uint32_t timestamp;
};

void mqtt_ota_init(void);
bool mqtt_ota_handle_command(const char* topic, const uint8_t* payload, unsigned int len);
void mqtt_ota_process(void);

void mqtt_ota_report_status(const char* target, const char* state, int progress,
                            const char* code, const char* detail);
void mqtt_ota_report_error(const char* target, const char* error_code,
                           const char* error_message);

bool mqtt_ota_is_busy(void);
const char* mqtt_ota_get_task_id(void);

#endif // MQTT_HANDLER_H
