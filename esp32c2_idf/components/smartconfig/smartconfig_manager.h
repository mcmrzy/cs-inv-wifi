#ifndef SMART_CONFIG_MANAGER_H
#define SMART_CONFIG_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_event.h"

typedef enum {
    SC_STATE_IDLE,
    SC_STATE_RUNNING,
    SC_STATE_DONE,
    SC_STATE_ERROR
} SmartConfigState;

typedef void (*SmartConfigCallback)(const char *ssid, const char *password);

void smartconfig_start(void);
void smartconfig_stop(void);
SmartConfigState smartconfig_get_state(void);
void smartconfig_set_callback(SmartConfigCallback cb);

void smartconfig_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *data);

#endif
