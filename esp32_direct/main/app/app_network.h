#pragma once
#include <stdbool.h>

/* Publish intent only. The main loop owns MQTT/BLE resource transitions.
 * Normal operation coexists with MQTT. Phone connection can request an
 * exclusive low-memory fallback; closing it allows an idle cloud return. */
void app_request_local_ble(bool requested);
