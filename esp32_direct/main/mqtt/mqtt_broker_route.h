#ifndef MQTT_BROKER_ROUTE_H
#define MQTT_BROKER_ROUTE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

const char *mqtt_broker_route_select(const char *configured_host,
                                     const char *default_host,
                                     const char *fallback_ip,
                                     bool use_fallback);

bool mqtt_broker_route_should_activate(bool fallback_allowed,
                                       bool cannot_resolve,
                                       bool already_active);

#ifdef __cplusplus
}
#endif

#endif
