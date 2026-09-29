#include "mqtt_envelope.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "failed at line %d: %s\n", __LINE__, #expr); \
        return 1; \
    } \
} while (0)

int main(void)
{
    char *payload = NULL;
    size_t length = 0;
    CHECK(mqtt_envelope_build("{\"ok\":true}", 123, 2, &payload, &length) == 0);
    CHECK(strcmp(payload, "{\"t\":123,\"v\":2,\"data\":{\"ok\":true}}") == 0);
    CHECK(length == strlen(payload));
    free(payload);

    char data[MQTT_ENVELOPE_MAX + 1];
    memset(data, 'x', sizeof(data) - 1);
    data[sizeof(data) - 1] = '\0';
    const size_t prefix = strlen("{\"t\":123,\"v\":2,\"data\":");
    const size_t largest_data = MQTT_ENVELOPE_MAX - 1 - prefix - 1;
    data[largest_data] = '\0';
    CHECK(mqtt_envelope_build(data, 123, 2, &payload, &length) == 0);
    CHECK(length == MQTT_ENVELOPE_MAX - 1);
    CHECK(payload[length] == '\0');
    free(payload);

    data[largest_data] = 'x';
    data[largest_data + 1] = '\0';
    payload = NULL;
    length = 0;
    CHECK(mqtt_envelope_build(data, 123, 2, &payload, &length) == -5);
    CHECK(payload == NULL);
    CHECK(length == 0);
    return 0;
}
