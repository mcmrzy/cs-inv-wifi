#include "mqtt_envelope.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int mqtt_envelope_build(const char *data, uint32_t timestamp, uint16_t version,
                        char **payload, size_t *length)
{
    if (!data || !payload || !length) return -2;
    *payload = NULL;
    *length = 0;

    char prefix[48];
    const int prefix_len = snprintf(prefix, sizeof(prefix),
                                    "{\"t\":%lu,\"v\":%u,\"data\":",
                                    (unsigned long)timestamp, (unsigned)version);
    if (prefix_len < 0 || prefix_len >= (int)sizeof(prefix)) return -5;

    const size_t data_len = strlen(data);
    const size_t header_len = (size_t)prefix_len;
    if (data_len > MQTT_ENVELOPE_MAX - header_len - 2) return -5;

    const size_t total = header_len + data_len + 1;
    char *buffer = malloc(total + 1);
    if (!buffer) return -6;

    memcpy(buffer, prefix, header_len);
    memcpy(buffer + header_len, data, data_len);
    buffer[header_len + data_len] = '}';
    buffer[total] = '\0';
    *payload = buffer;
    *length = total;
    return 0;
}
