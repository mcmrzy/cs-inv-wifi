#include "ota_health.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
int main(void){
 assert(direct_ota_health_missing(10000,9900,2000,true,true,true,true)==0);
 assert(direct_ota_health_missing(10000,0,2000,true,true,true,true)==OTA_HEALTH_MAIN);
 assert(direct_ota_health_missing(10000,7999,2000,true,true,true,true)==OTA_HEALTH_MAIN);
 assert(direct_ota_health_missing(10000,9900,2000,false,true,true,true)==OTA_HEALTH_DISPLAY);
 assert(direct_ota_health_missing(10000,9900,2000,true,false,true,true)==OTA_HEALTH_LINK);
 assert(direct_ota_health_missing(10000,9900,2000,true,true,false,true)==OTA_HEALTH_USB);
 assert(direct_ota_health_missing(10000,9900,2000,true,true,true,false)==OTA_HEALTH_IDENTITY);
 assert(direct_ota_health_missing(50,UINT32_MAX-50,2000,true,true,true,true)==0);
 assert(direct_ota_health_missing(10000,0,2000,false,false,false,false)==OTA_HEALTH_ALL);
 puts("PASS OTA startup health: all software services required, stale/wrap/missing identity");
}
