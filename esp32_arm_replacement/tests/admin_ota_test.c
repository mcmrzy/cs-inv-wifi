#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define pdTRUE 1
#define portMAX_DELAY 1000
#define pdMS_TO_TICKS(ms) (ms)
typedef enum {OTA_RESULT_OK,OTA_RESULT_INTERNAL,OTA_RESULT_BUSY,OTA_RESULT_DUPLICATE} OtaResult;
typedef struct {char task_id[20];} OtaManifest;
static OtaManifest g_snapshot;
static bool g_active,g_admin_busy,g_remote_cleanup_busy,locked;
static int g_lock=1;
static int xSemaphoreTake(int lock,int ticks){(void)ticks;assert(lock&&!locked);locked=true;return pdTRUE;}
static void xSemaphoreGive(int lock){assert(lock&&locked);locked=false;}
#include "../tmp/admin_ota_under_test.inc"
int main(void){
 OtaManifest m={"new"};g_lock=0;assert(!ota_service_admin_begin());
 g_lock=1;assert(ota_service_admin_begin());assert(!locked);
 assert(!ota_service_admin_begin());assert(check_available(&m)==OTA_RESULT_BUSY);
 ota_service_admin_end();assert(check_available(&m)==OTA_RESULT_OK);
 g_active=true;assert(!ota_service_admin_begin());assert(check_available(&m)==OTA_RESULT_BUSY);
 strcpy(g_snapshot.task_id,"new");assert(check_available(&m)==OTA_RESULT_DUPLICATE);
 g_active=false;g_remote_cleanup_busy=true;
 assert(!ota_service_admin_begin());assert(check_available(&m)==OTA_RESULT_DUPLICATE);
 strcpy(g_snapshot.task_id,"old");assert(check_available(&m)==OTA_RESULT_BUSY);
 g_remote_cleanup_busy=false;assert(check_available(&m)==OTA_RESULT_OK);
 assert(!g_admin_busy&&!locked);
 puts("PASS actual admin/OTA lease: startup, reentry, mutual exclusion, unlock and duplicate semantics");
 return 0;
}
