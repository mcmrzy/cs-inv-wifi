#ifndef BATTERY_COMMAND_HOST_H
#define BATTERY_COMMAND_HOST_H
/* Existing scalar-command tests do not start a battery transaction. Keep this
 * boundary unavailable; grouped tests inject an observable host-only writer. */
#include "../main/direct/direct_battery_config.h"
static bool direct_battery_config_get(direct_battery_config_t *out) { (void)out; return false; }
static int direct_battery_config_write(const direct_battery_config_t *config, bool cloud) {
    (void)config; (void)cloud; return -13;
}
#endif
