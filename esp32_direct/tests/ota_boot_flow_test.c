#include "ota_health.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define OTA_HEALTH_DELAY_MS 10000U
#define OTA_HEALTH_MAX_AGE_MS 2000U
#define OTA_TASK_STACK_SIZE 12288U
#define pdMS_TO_TICKS(x) (x)
#define ESP_OK 0
#define ESP_LOGE(tag,fmt,value) ((void)(tag),(void)(fmt),(void)(value))
typedef int esp_err_t;
static const char*esp_err_to_name(esp_err_t err){(void)err;return "SIMULATED";}
typedef struct {unsigned security_version;} StorageOtaPending;
typedef struct {char unused;} factory_record_t;
typedef enum {FACTORY_OK,FACTORY_UNPROVISIONED,FACTORY_CORRUPT} factory_result_t;
typedef struct {unsigned total;} OtaSnapshot;
enum {OTA_STATE_FAILED,OTA_STATE_SUCCEEDED,OTA_RESULT_INTERNAL,OTA_RESULT_PERSIST_FAILED,OTA_RESULT_OK};
static StorageOtaPending g_boot_pending;
static atomic_uint g_main_loop_tick;
static bool g_boot_validation_required;
static bool panel,link,usb,save_ok,clear_ok,confirm_ok;
static factory_result_t identity;
static unsigned valid_calls,rollback_calls,save_calls,clear_calls,release_calls,last_state;
static uint32_t xTaskGetTickCount(void){return 10000;}
static void vTaskDelay(unsigned x){(void)x;}
static factory_result_t factory_identity_read(factory_record_t*r){(void)r;return identity;}
static bool direct_panel_software_healthy(void){return panel;}
static bool direct_link_software_healthy(void){return link;}
static bool factory_service_software_healthy(void){return usb;}
static void update_snapshot(unsigned state,unsigned progress,unsigned total,unsigned result,const char*message){(void)progress;(void)total;(void)result;(void)message;last_state=state;}
static esp_err_t esp_ota_mark_app_valid_cancel_rollback(void){valid_calls++;return confirm_ok?ESP_OK:-1;}
static esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void){rollback_calls++;return -1;}
static bool storage_set_ota_security_version_for_target(const char*t,unsigned v){(void)t;(void)v;save_calls++;return save_ok;}
static bool storage_clear_ota_pending(void){clear_calls++;return clear_ok;}
static bool ota_service_get_snapshot(OtaSnapshot*s){s->total=1024;return true;}
static void release_active(void){release_calls++;}
/* Reproduce the supplied post-reboot heap: 15KB free, largest block 7680B.
 * Confirmation must finish even when a new TLS-sized task cannot be created. */
#define create_ota_task_with_retry(entry,name,stack,param,handle) ((stack)<=7680U)
#include "../tmp/ota_boot_under_test.inc"
static void reset(void){panel=link=usb=save_ok=clear_ok=confirm_ok=true;identity=FACTORY_OK;valid_calls=rollback_calls=save_calls=clear_calls=release_calls=0;g_boot_pending.security_version=0;g_boot_validation_required=true;atomic_store(&g_main_loop_tick,9900);}
static void failed(void){ota_service_start_boot_health_check();assert(valid_calls==0&&rollback_calls==1&&clear_calls==0&&last_state==OTA_STATE_FAILED);}
int main(void){
 reset();assert(ota_service_start_boot_health_check());
 assert(valid_calls==1 && last_state==OTA_STATE_SUCCEEDED && release_calls==1);
 reset();g_boot_validation_required=false;assert(ota_service_start_boot_health_check());
 assert(valid_calls==0 && clear_calls==0);
 reset();panel=false;failed();reset();link=false;failed();reset();usb=false;failed();
 reset();identity=FACTORY_CORRUPT;failed();reset();atomic_store(&g_main_loop_tick,0);failed();
 reset();ota_service_start_boot_health_check();assert(valid_calls==1&&rollback_calls==0&&clear_calls==1&&save_calls==0&&release_calls==1&&!g_boot_validation_required&&last_state==OTA_STATE_SUCCEEDED);
 reset();g_boot_pending.security_version=2;ota_service_start_boot_health_check();assert(save_calls==1&&clear_calls==1&&last_state==OTA_STATE_SUCCEEDED);
 reset();clear_ok=false;ota_service_start_boot_health_check();assert(last_state==OTA_STATE_FAILED);
 reset();confirm_ok=false;ota_service_start_boot_health_check();assert(last_state==OTA_STATE_FAILED && clear_calls==0 && release_calls==0);
 reset();g_boot_pending.security_version=2;save_ok=false;ota_service_start_boot_health_check();assert(last_state==OTA_STATE_FAILED && clear_calls==0 && release_calls==0);
 reset();identity=FACTORY_UNPROVISIONED;
#if CONFIG_DIRECT_IDENTITY_PRODUCTION
 failed();
#else
 ota_service_start_boot_health_check();assert(valid_calls==1&&last_state==OTA_STATE_SUCCEEDED);
#endif
 puts("PASS actual OTA boot flow: readiness gates, rollback request, zero-version cleanup, identity policy");
}
