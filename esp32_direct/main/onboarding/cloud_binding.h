#pragma once
#include <stdbool.h>
#include <stddef.h>
typedef enum { CLOUD_BINDING_UNKNOWN, CLOUD_BINDING_UNBOUND, CLOUD_BINDING_BOUND } cloud_binding_state_t;
bool cloud_binding_start(void);
cloud_binding_state_t cloud_binding_get(void);
void cloud_binding_receive(const char *topic,const char *payload,size_t length);
/* MQTT owner sets true only after the matching binding/state SUBACK. */
void cloud_binding_subscription(bool ready);
