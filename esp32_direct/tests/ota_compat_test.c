#include "ota_compat.h"
#include <assert.h>
#include <stdio.h>
int main(void){assert(direct_ota_compatible(false,"","","CS-L10-6K2","UNSET"));assert(!direct_ota_compatible(true,"","","CS-L10-6K2","A1"));assert(direct_ota_compatible(true,"CS-L10-6K2","A1","CS-L10-6K2","A1"));assert(!direct_ota_compatible(true,"Other","A1","CS-L10-6K2","A1"));assert(!direct_ota_compatible(true,"CS-L10-6K2","A2","CS-L10-6K2","A1"));assert(!direct_ota_compatible(true,"CS-L10-6K2","UNSET","CS-L10-6K2","UNSET"));puts("PASS OTA model/hardware fail-closed compatibility");}
