#include "ota_service.h"
#include "direct_iap.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
typedef unsigned esp_ota_handle_t;
typedef void *esp_http_client_handle_t;
typedef bool (*OtaResumeFn)(void*,uint32_t);
static FILE *g_direct_stage;
static OtaManifest g_manifest;
static char g_arm_failure_detail[128];
static bool cancelled,space=true,persist=true;
#define ESP_LOGW(...) ((void)0)
static void mqtt_publish_ota_trace(const char*a,const char*b){assert(a&&b);}
static unsigned uart_jobs,closed,quiet_on,quiet_off,released,succeeded,esp_jobs;
static unsigned persisted_versions;
static OtaResult stage_result=OTA_RESULT_OK;
static direct_iap_result_t wire_result=DIRECT_IAP_OK;
static bool cancel_requested(void){return cancelled;}
static bool offline_cache_reserve_space(size_t n){assert(n==g_manifest.file_size+16384);return space;}
const char *ota_target_name(OtaTarget t){return t==OTA_TARGET_DSP?"dsp":"bms";}
static void esp_http_client_close(void *c){assert(c==(void*)1);closed++;}
static void ota_quiet_begin(void){quiet_on++;}
static void ota_quiet_end(void){quiet_off++;}
static void restore_wifi_ps(void){}
static void release_active(void){released++;}
static bool storage_set_ota_security_version_for_target(const char *t,uint32_t n){assert(t&&n);persisted_versions++;return persist;}
/* 每个阶段一条独立进度条(progress == stage_progress), 阶段名用 App 已有的
 * transferring/verifying/succeeded, 且传输阶段含等待 HR/擦除。顺序被记录,
 * 防止再退回"下载报 0..100、安装又跳回 70"的写法。 */
typedef struct {int unused;} RunParamDef;
typedef struct {int unused;} direct_bms_measurement_t;
/* 生产实现里的同名常量(被抽取进 .inc 的函数直接引用) */
#define OTA_TARGET_RESPONSE_WAIT_MS 8000U
#define OTA_TARGET_RESPONSE_POLL_MS 200U
#define pdMS_TO_TICKS(ms) (ms)
static uint32_t fake_ms;
static bool target_responds=true;
static int64_t esp_timer_get_time(void){return (int64_t)fake_ms*1000;}
static void vTaskDelay(unsigned ms){fake_ms+=ms;}
static bool direct_dsp_get_measurement(RunParamDef*m,uint32_t*received_ms){
 assert(m&&received_ms);*received_ms=fake_ms;return target_responds;}
static bool direct_bms_get_snapshot(direct_bms_measurement_t*m,uint32_t*age_ms){
 assert(m&&age_ms);*age_ms=0;return target_responds;}
static char stage_log[512];
static char last_logged_stage[16];
static void update_snapshot_stage(OtaState s,uint8_t p,uint32_t n,OtaResult r,
                                  const char *message,const char *stage,
                                  uint8_t stage_progress){
 assert(n==g_manifest.file_size&&r==OTA_RESULT_OK&&p<=100&&message&&stage);
 assert(p==stage_progress&&stage_progress<=100);
 assert(!strcmp(stage,"transferring")||!strcmp(stage,"verifying")||
        !strcmp(stage,"succeeded"));
 if(s==OTA_STATE_SUCCEEDED){
  assert(!strcmp(stage,"succeeded")&&p==100&&stage_progress==100);
  assert(strstr(message,"Target transfer complete"));
  assert(target_responds&&strstr(message,"application responded"));
  succeeded++;
 }
 /* 同一阶段的多次进度刷新只记一次, 记的是阶段顺序 */
 if(strcmp(stage,last_logged_stage)){
  snprintf(last_logged_stage,sizeof(last_logged_stage),"%s",stage);
  strncat(stage_log,stage,sizeof(stage_log)-strlen(stage_log)-1);
  strncat(stage_log,";",sizeof(stage_log)-strlen(stage_log)-1);
 }
}
static OtaResult read_and_stage(const OtaManifest *m,OtaStreamReader r,void *c,esp_ota_handle_t *h,OtaResumeFn resume,unsigned attempts){
 (void)r;(void)c;(void)h;assert(!resume&&!attempts);
 for(size_t i=0;i<m->file_size;i++)assert(fputc((int)(i&255),g_direct_stage)!=EOF);
 return stage_result;
}
static direct_iap_result_t direct_link_run_iap(const direct_iap_image_t *m,const direct_iap_io_t *io){
 uart_jobs++;assert(quiet_on==quiet_off+1&&g_direct_stage);
 assert(m->target==(direct_iap_target_t)g_manifest.target&&m->size==g_manifest.file_size);
 assert(!m->hw_type&&!m->hw_model&&!m->customer);
 assert(!io->verify_identity&&!io->verify_version);
#if CONFIG_DIRECT_DSP_OTA_PACKET0_PROBE
 assert(io->probe_packet0==(m->target==DIRECT_IAP_DSP));
#else
 assert(!io->probe_packet0);
#endif
 uint8_t b[1030];assert(io->read_image(io->context,0,b,sizeof(b)));
 for(size_t i=0;i<sizeof(b);i++)assert(b[i]==(uint8_t)i);
 assert(io->cancelled(io->context)==cancelled);
 io->progress(io->context,m->size,m->size);
 return wire_result;
}
static OtaResult install_esp(const OtaManifest *m,OtaStreamReader r,void*c,OtaResumeFn f,unsigned n){(void)m;(void)r;(void)c;(void)f;(void)n;esp_jobs++;return OTA_RESULT_OK;}
/* Redirect only the fixed staging pathname; all real file I/O remains intact. */
static FILE *test_fopen(const char *p,const char *m){assert(!strcmp(p,"/cache/direct_target_ota.bin"));return fopen("tmp/direct_target_flow.bin",m);}
static int test_remove(const char *p){assert(!strcmp(p,"/cache/direct_target_ota.bin"));return remove("tmp/direct_target_flow.bin");}
#define fopen test_fopen
#define remove test_remove
#include "../tmp/direct_ota_under_test.inc"
#undef fopen
#undef remove
static int reader(void*c,uint8_t*b,size_t n){(void)c;(void)b;(void)n;return 0;}
#if CONFIG_DIRECT_DSP_OTA_PACKET0_PROBE
int main(void){
 g_manifest=(OtaManifest){.target=OTA_TARGET_DSP,.file_size=2048,.source=OTA_SOURCE_LOCAL};
 wire_result=DIRECT_IAP_DIAGNOSTIC_COMPLETE;
 assert(install_stream(&g_manifest,reader,NULL,NULL,0,NULL)==OTA_RESULT_DIAGNOSTIC_ONLY);
 assert(uart_jobs==1&&!succeeded&&!released&&quiet_on==quiet_off&&!g_direct_stage);
 assert(strstr(g_arm_failure_detail,"PACKET0_DIAGNOSTIC_COMPLETE"));
 assert(!strstr(stage_log,"verifying")&&!strstr(stage_log,"succeeded"));
 assert(!persisted_versions);
 puts("PASS diagnostic OTA service: DIAGNOSTIC_ONLY returned, no verifying/succeeded or version persistence");
}
#else
int main(void){
 assert(!strcmp(ota_result_code(OTA_RESULT_TARGET_RESPONSE_TIMEOUT),"TARGET_RESPONSE_TIMEOUT"));
 assert(!strcmp(ota_result_code(OTA_RESULT_TARGET_VERSION_UNCONFIRMED),"TARGET_VERSION_UNCONFIRMED"));
 assert(!strcmp(ota_result_code(OTA_RESULT_DIAGNOSTIC_ONLY),"DIAGNOSTIC_ONLY"));
 assert(!strcmp(ota_result_code(OTA_RESULT_TASK_REPLAY_BLOCKED),"TASK_REPLAY_BLOCKED"));
 g_manifest=(OtaManifest){.target=OTA_TARGET_DSP,.file_size=2048,.source=OTA_SOURCE_MQTT};strcpy(g_manifest.version,"test");
 bool source_closed=false;
 assert(install_stream(&g_manifest,reader,(void*)1,NULL,0,&source_closed)==OTA_RESULT_OK);
 assert(source_closed&&closed==1&&uart_jobs==1&&succeeded==1&&released==1&&quiet_on==quiet_off&&!g_direct_stage);
 assert(strstr(stage_log,"transferring;verifying;succeeded;"));
 g_manifest.target=OTA_TARGET_BMS;g_manifest.source=OTA_SOURCE_LOCAL;
 assert(install_stream(&g_manifest,reader,NULL,NULL,0,NULL)==OTA_RESULT_OK&&uart_jobs==2&&succeeded==2);
 stage_result=OTA_RESULT_HASH_MISMATCH;
 assert(install_stream(&g_manifest,reader,NULL,NULL,0,NULL)==OTA_RESULT_HASH_MISMATCH&&uart_jobs==2&&!g_direct_stage);
 stage_result=OTA_RESULT_OK;wire_result=DIRECT_IAP_ACK_FAILED;
 assert(install_stream(&g_manifest,reader,NULL,NULL,0,NULL)==OTA_RESULT_BMS_TRANSFER_FAILED&&succeeded==2&&quiet_on==quiet_off);
 g_manifest.target=OTA_TARGET_DSP;
 assert(install_stream(&g_manifest,reader,NULL,NULL,0,NULL)==OTA_RESULT_DSP_TRANSFER_FAILED&&strstr(g_arm_failure_detail,"ACK_TIMEOUT"));
 wire_result=DIRECT_IAP_IMAGE_MAGIC_INVALID;
 assert(install_stream(&g_manifest,reader,NULL,NULL,0,NULL)==OTA_RESULT_IMAGE_FORMAT_INVALID&&strstr(g_arm_failure_detail,"IMAGE_MAGIC_INVALID"));
 wire_result=DIRECT_IAP_BUSY;
 assert(install_stream(&g_manifest,reader,NULL,NULL,0,NULL)==OTA_RESULT_BUSY&&strstr(g_arm_failure_detail,"UART_JOB_BUSY"));
 wire_result=DIRECT_IAP_NOT_READY;
 assert(install_stream(&g_manifest,reader,NULL,NULL,0,NULL)==OTA_RESULT_TARGET_NOT_READY);
 wire_result=DIRECT_IAP_OK;g_manifest.security_version=2;persist=false;
 assert(install_stream(&g_manifest,reader,NULL,NULL,0,NULL)==OTA_RESULT_PERSIST_FAILED&&succeeded==2);
 assert(persisted_versions==1);
 g_manifest.target=OTA_TARGET_BMS;
 assert(install_stream(&g_manifest,reader,NULL,NULL,0,NULL)==OTA_RESULT_PERSIST_FAILED&&succeeded==2);
 cancelled=true;assert(install_stream(&g_manifest,reader,NULL,NULL,0,NULL)==OTA_RESULT_OK&&released==2);
 cancelled=false;space=false;
 assert(install_stream(&g_manifest,reader,NULL,NULL,0,NULL)==OTA_RESULT_INSUFFICIENT_STORAGE&&!g_direct_stage);
 g_manifest.target=OTA_TARGET_ESP;
 assert(install_stream(&g_manifest,reader,NULL,NULL,0,NULL)==OTA_RESULT_OK&&esp_jobs==1);
 /* Protocol completion without a restarted target must never report success. */
 g_manifest.target=OTA_TARGET_DSP;g_manifest.source=OTA_SOURCE_MQTT;
 g_manifest.security_version=0;persist=true;space=true;
 target_responds=false;
 last_logged_stage[0]='\0';   /* 让本次安装的阶段序列从 transferring 开始记录 */
 const unsigned uart_before=uart_jobs, succ_before=succeeded, log_before=strlen(stage_log);
 assert(install_stream(&g_manifest,reader,NULL,NULL,0,NULL)==OTA_RESULT_TARGET_RESPONSE_TIMEOUT&&
        uart_jobs==uart_before+1&&succeeded==succ_before&&quiet_on==quiet_off);
 assert(strstr(stage_log+log_before,"transferring;verifying;"));
 target_responds=true;
 puts("PASS real OTA DSP/BMS routing, raw staging, hash gate, Boot failure, cleanup, persistence, per-stage progress bars, ESP isolation");
}
#endif
