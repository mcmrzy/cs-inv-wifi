#include "mqtt_broker_route.h"

#include <string.h>

const char *mqtt_broker_route_select(const char *configured_host,
                                     const char *default_host,
                                     const char *fallback_ip,
                                     bool use_fallback)
{
    if (use_fallback && configured_host && default_host && fallback_ip &&
        fallback_ip[0] != '\0' && strcmp(configured_host, default_host) == 0) {
        return fallback_ip;
    }
    return configured_host;
}

bool mqtt_broker_route_should_activate(bool fallback_allowed,
                                       bool cannot_resolve,
                                       bool already_active)
{
    return fallback_allowed && cannot_resolve && !already_active;
}
