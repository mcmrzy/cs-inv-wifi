#pragma once
#include <stdbool.h>
#include <stdint.h>
enum {OTA_HEALTH_MAIN=1,OTA_HEALTH_DISPLAY=2,OTA_HEALTH_LINK=4,
      OTA_HEALTH_USB=8,OTA_HEALTH_IDENTITY=16,OTA_HEALTH_ALL=31};
/* Software readiness only. Offline peripherals and physical power/display
 * acceptance must be evaluated separately by the manufacturing procedure. */
uint32_t direct_ota_health_missing(uint32_t now,uint32_t main_tick,uint32_t max_age,
 bool display,bool link_tasks,bool usb,bool identity);
