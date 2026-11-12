#pragma once
#include <stdbool.h>
bool full_alarm_init(void);
void full_alarm_tick(void);
bool full_alarm_needs_attention(void);
/* Called only by the MQTT event task; these never format, flash or publish. */
void full_alarm_transport_reset(void);
void full_alarm_puback(int message_id);
void full_alarm_expired(int message_id);
/* Full MQTT adapter accepts an already-formed strict V1 envelope. */
int full_alarm_mqtt_enqueue(const char *envelope);
