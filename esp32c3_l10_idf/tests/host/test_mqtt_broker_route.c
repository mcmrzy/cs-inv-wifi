#include "mqtt_broker_route.h"

#include <stdio.h>
#include <string.h>

static int expect_host(const char *actual, const char *expected,
                       const char *scenario)
{
    if (actual && strcmp(actual, expected) == 0) return 0;
    fprintf(stderr, "%s: expected %s, got %s\n", scenario, expected,
            actual ? actual : "(null)");
    return 1;
}

int main(void)
{
    const char *default_host = "mqtt.jiuxiaoyw.online";
    const char *fallback_ip = "101.43.90.25";

    if (expect_host(mqtt_broker_route_select(default_host, default_host,
                                             fallback_ip, false),
                    default_host, "normal DNS route")) return 1;
    if (expect_host(mqtt_broker_route_select(default_host, default_host,
                                             fallback_ip, true),
                    fallback_ip, "default broker DNS failure")) return 1;
    if (expect_host(mqtt_broker_route_select("custom.example.com", default_host,
                                             fallback_ip, true),
                    "custom.example.com", "custom broker DNS failure")) return 1;
    if (!mqtt_broker_route_should_activate(true, true, false)) {
        fprintf(stderr, "default broker resolution failure must activate fallback\n");
        return 1;
    }
    if (mqtt_broker_route_should_activate(false, true, false) ||
        mqtt_broker_route_should_activate(true, false, false) ||
        mqtt_broker_route_should_activate(true, true, true)) {
        fprintf(stderr, "fallback activated for an ineligible error or route\n");
        return 1;
    }
    return 0;
}
