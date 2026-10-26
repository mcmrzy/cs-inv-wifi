#include "ota_health.h"
uint32_t direct_ota_health_missing(uint32_t now,uint32_t main_tick,uint32_t max_age,
 bool display,bool link_tasks,bool usb,bool identity){
 uint32_t missing=0;
 if(!main_tick||now-main_tick>max_age)missing|=OTA_HEALTH_MAIN;
 if(!display)missing|=OTA_HEALTH_DISPLAY;
 if(!link_tasks)missing|=OTA_HEALTH_LINK;
 if(!usb)missing|=OTA_HEALTH_USB;
 if(!identity)missing|=OTA_HEALTH_IDENTITY;
 return missing;
}
