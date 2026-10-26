#ifndef MQTT_ENVELOPE_H
#define MQTT_ENVELOPE_H

#include <stddef.h>
#include <stdint.h>

/* The expanded identity/board INFO must fit with its timestamp envelope. */
#define MQTT_ENVELOPE_MAX 4096U

/* Caller owns *payload on success and must free it after publishing. */
int mqtt_envelope_build(const char *data, uint32_t timestamp, uint16_t version,
                        char **payload, size_t *length);

#endif
