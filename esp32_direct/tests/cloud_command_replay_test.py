"""Run the actual MQTT cJSON dispatcher, callbacks and SHA256 on the host."""
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
GCC = Path(r'C:\Users\29538\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe')
JSON = Path(r'D:\.espressif\v5.5.4\esp-idf\components\json\cJSON')
MBEDTLS = JSON.parents[1] / 'mbedtls/mbedtls'
source = (ROOT / 'main/telemetry/cmd_handler.c').read_text(encoding='utf-8')
source = re.sub(r'^#include "[^\n]+"\s*$', '', source, flags=re.M)
arm_header = (ROOT / 'main/telemetry/arm_poll.h').read_text(encoding='utf-8')
source = re.search(r'typedef enum \{\s*ARM_WRITE_ACCEPTED.*?\} arm_write_result_t;', arm_header, re.S).group() + '\n' + source
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include "cJSON.h"
#include "mbedtls/sha256.h"
#include "cs_protocol_defs.h"
#include "cs_protocol.h"
#include "cmd_handler.h"
#include "arm_param.h"
#include "../tests/battery_command_host.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define DIRECT_DSP_CONFIG_COUNT 84
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(p) pthread_mutex_lock(p)
#define portEXIT_CRITICAL(p) pthread_mutex_unlock(p)
static pthread_mutex_t publish_lock=PTHREAD_MUTEX_INITIALIZER;
static int64_t clock_ms=1000;
static time_t wall=1800000000;
static bool synced=true,queued,synchronous,fail_enqueue;
static int enqueue_error;
static uint16_t addr,raw,stored[84];
static unsigned writes,queries,debug_calls;
static char response[1024];
static bool allocation_failure;
static void *test_malloc(size_t n){return allocation_failure ? NULL : malloc(n);}
static struct {uint32_t CtrlParamAlterTime;} g_read_write={5};
static int64_t esp_timer_get_time(void){return clock_ms*1000;}
static bool sntp_sync_is_synced(void){return synced;}
static time_t test_time(time_t *t){if(t)*t=wall;return wall;}
#define time test_time
static bool arm_poll_take_param(void){return true;}
static void arm_poll_give_param(void){}
static bool direct_config_get_wire(uint16_t i,uint16_t *out){if(i>=84)return false;*out=stored[i];return true;}
static void (*write_cb)(uint8_t,uint16_t,uint16_t,int);
static void arm_poll_set_write_cb(void (*cb)(uint8_t,uint16_t,uint16_t,int)){write_cb=cb;}
static int arm_poll_send_write(uint8_t cmd,uint16_t i,uint16_t n,const uint8_t *data){
 assert(cmd==CS_CMD_WRITE_CTRLPARAM && n==2);if(enqueue_error)return enqueue_error;if(queued||fail_enqueue)return -1;
 writes++;queued=true;addr=i;raw=data[0]|((uint16_t)data[1]<<8);
 if(synchronous){queued=false;stored[addr]=raw;write_cb(cmd,addr,2,0);}return 0;
}
static int mqtt_publish(const char *topic,const char *body,int len,int qos,int retain){
 (void)topic;(void)len;assert(qos==1 && retain==0);pthread_mutex_lock(&publish_lock);snprintf(response,sizeof(response),"%s",body);pthread_mutex_unlock(&publish_lock);
 if(!allocation_failure){cJSON *j=cJSON_Parse(body);assert(j);cJSON_Delete(j);}return 0;
}
static void telemetry_request_config(void){queries++;}
static void telemetry_request_info(void){queries++;}
static void telemetry_request_heartbeat(void){queries++;}
static void telemetry_debug_set(bool enabled,uint32_t interval,uint32_t duration,uint32_t *effective,int64_t *expires){
 (void)enabled;(void)duration;debug_calls++;*effective=interval;*expires=wall+duration;
}
static uint32_t ble_ct_cmd_write_token(void){return 123;}
static void ble_ct_cmd_on_write_ack(uint32_t token,const char *id,int err){(void)token;(void)id;(void)err;}
static void ble_ct_cmd_on_write_saved(uint32_t token,const char *id,double value,uint32_t revision){(void)token;(void)id;(void)value;(void)revision;}
SOURCE
static void request(const char *json){cmd_handler_on_mqtt_message("cs_inv/TEST/cmd",json,(unsigned)strlen(json));cmd_handler_tick();}
static void finish(int err){assert(queued);queued=false;stored[addr]=raw;write_cb(CS_CMD_WRITE_CTRLPARAM,addr,2,err);}
static void expect(const char *key,const char *value){cJSON *j=cJSON_Parse(response);assert(j);cJSON *v=cJSON_GetObjectItem(j,key);assert(cJSON_IsString(v) && strcmp(v->valuestring,value)==0);cJSON_Delete(j);}
static void *concurrent_request(void *json){cmd_handler_on_mqtt_message("cs_inv/TEST/cmd",json,(unsigned)strlen(json));return NULL;}
int main(void){
 cJSON_Hooks hooks={test_malloc,free};cJSON_InitHooks(&hooks);
 cmd_handler_init();
 const char *first="{\"task_id\":\"a\",\"cmd\":\"set_output_voltage\",\"args\":[230],\"expires_at\":1800000030}";
 request(first);assert(writes==1);finish(0);request(first);
 assert(writes==1); /* RED: original dispatcher writes twice. */
 expect("stage","submitted");assert(!strstr(response,"applied_args") && !strstr(response,"reported_revision"));
 request("{\"task_id\":\"a\",\"cmd\":\"set_output_voltage\",\"args\":[220]}");expect("result","IDEMPOTENCY_CONFLICT");assert(writes==1);
 request("{\"task_id\":\"expired\",\"cmd\":\"set_output_voltage\",\"args\":[230],\"expires_at\":1800000000}");expect("result","EXPIRED");assert(writes==1);
 synced=false;request("{\"task_id\":\"clock\",\"cmd\":\"set_output_voltage\",\"args\":[230],\"expires_at\":1800000030}");expect("result","CLOCK_UNSYNCED");assert(writes==1);synced=true;
 request("{\"task_id\":\"badtime\",\"cmd\":\"set_output_voltage\",\"args\":[230],\"expires_at\":\"tomorrow\"}");expect("result","INVALID_ARGS");assert(writes==1);
 request("{\"task_id\":\"b\",\"cmd\":\"set_output_voltage\",\"args\":[230]}");expect("stage","acknowledged");request("{\"cmd\":\"set_output_voltage\",\"args\":[230],\"task_id\":\"b\",\"t\":1}");assert(writes==2);finish(-1);expect("stage","failed");
 request("{\"task_id\":\"b\",\"cmd\":\"set_output_voltage\",\"args\":[230]}");expect("result","EXEC_FAILED");assert(writes==2);
 cmd_handler_init();synchronous=true;request(first);expect("stage","submitted");assert(writes==3);synchronous=false;
 request("{\"task_id\":\"quote\\\"\\\\id\",\"cmd\":\"query_config\"}");expect("stage","completed");expect("task_id","quote\"\\id");
 unsigned before=queries;request("{\"task_id\":\"quote\\\"\\\\id\",\"cmd\":\"query_config\"}");assert(queries==before+1);
 request("{\"task_id\":\"debug\",\"cmd\":\"set_debug_telemetry\",\"args\":[1,5,60]}");request("{\"task_id\":\"debug\",\"cmd\":\"set_debug_telemetry\",\"args\":[1,5,60]}");assert(debug_calls==1);
 request("{\"task_id\":\"debug\",\"cmd\":\"set_debug_telemetry\",\"args\":[1,5,61]}");expect("result","IDEMPOTENCY_CONFLICT");
 cmd_handler_init();for(unsigned i=0;i<8;i++){char msg[128];snprintf(msg,sizeof(msg),"{\"task_id\":\"depth-%u\",\"cmd\":\"set_output_voltage\",\"value\":230}",i);request(msg);finish(0);}
 unsigned depth_writes=writes;
 request("{\"task_id\":\"full\",\"cmd\":\"set_output_voltage\",\"args\":[230]}");expect("result","BUSY");assert(writes==depth_writes);
 clock_ms+=600001;request("{\"task_id\":\"full\",\"cmd\":\"set_output_voltage\",\"args\":[230]}");assert(writes==depth_writes+1);finish(0);
 request("{\"cmd\":\"set_output_voltage\",\"value\":230}");assert(writes==depth_writes+2);finish(0);expect("stage","submitted");
 cmd_handler_init();request("{\"task_id\":\"heap\",\"cmd\":\"set_output_voltage\",\"args\":[230.05]}");
 allocation_failure=true;finish(0);allocation_failure=false;expect("stage","submitted");
 cJSON *quantized=cJSON_Parse(response);assert(cJSON_GetArrayItem(cJSON_GetObjectItem(quantized,"submitted_args"),0)->valuedouble==230.0);cJSON_Delete(quantized);
 unsigned count=writes;request("{\"task_id\":\"heap\",\"cmd\":\"set_output_voltage\",\"args\":[230.05]}");assert(writes==count);expect("stage","submitted");
 cmd_handler_init();request("{\"task_id\":\"pending\",\"cmd\":\"set_output_voltage\",\"args\":[230]}");
 clock_ms+=600001;request("{\"task_id\":\"pending\",\"cmd\":\"set_output_voltage\",\"args\":[220]}");expect("result","IDEMPOTENCY_CONFLICT");
 count=writes;request("{\"task_id\":\"pending\",\"cmd\":\"set_output_voltage\",\"args\":[230]}");assert(writes==count);expect("stage","acknowledged");finish(0);
 request("{\"task_id\":\"pending\",\"cmd\":\"set_output_voltage\",\"args\":[230]}");assert(writes==count);expect("stage","submitted");
 fail_enqueue=true;request("{\"task_id\":\"busy\",\"cmd\":\"set_output_voltage\",\"args\":[230]}");expect("result","BUSY");fail_enqueue=false;
 request("{\"task_id\":\"busy\",\"cmd\":\"set_output_voltage\",\"args\":[230]}");assert(writes==count+1);finish(0);expect("stage","submitted");
 cmd_handler_init();count=writes;pthread_t threads[16];
 for(unsigned i=0;i<16;i++)assert(pthread_create(&threads[i],NULL,concurrent_request,(void *)first)==0);
 for(unsigned i=0;i<16;i++)assert(pthread_join(threads[i],NULL)==0);
 for(unsigned i=0;i<CMD_HANDLER_QUEUE_DEPTH;i++)cmd_handler_tick();
 assert(writes==count+1);finish(0);request(first);expect("stage","submitted");assert(writes==count+1);
 cmd_handler_init();char longest[65];memset(longest,'"',64);longest[64]=0;cJSON *long_request=cJSON_CreateObject();
 cJSON_AddStringToObject(long_request,"task_id",longest);cJSON_AddStringToObject(long_request,"cmd","set_output_voltage");cJSON_AddNumberToObject(long_request,"value",230);
 char *long_json=cJSON_PrintUnformatted(long_request);request(long_json);finish(-1);expect("result","EXEC_FAILED");expect("task_id",longest);request(long_json);expect("result","EXEC_FAILED");cJSON_free(long_json);cJSON_Delete(long_request);
 count=writes;request("{\"task_id\":\"control\\u0001\",\"cmd\":\"set_output_voltage\",\"args\":[230]}");expect("result","INVALID_ARGS");assert(writes==count);
 puts("PASS actual MQTT dispatcher: 16 concurrent duplicates, pending/conflict, expired/clock, terminal replay under allocation failure, quantized submitted value, longest escaped ID, cache full/TTL, legacy, submitted truth");
}
'''.replace('SOURCE', source)
target = ROOT / 'tmp/cloud_command_replay_test.c'
target.parent.mkdir(exist_ok=True)
target.write_text(harness, encoding='utf-8')
config = target.parent / 'cloud_sha256_host_config.h'
config.write_text('#define MBEDTLS_SHA256_C\n#define MBEDTLS_PLATFORM_C\n', encoding='utf-8')
exe = target.with_suffix('.exe')
subprocess.run([str(GCC), '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread', '-static', '-Wno-unused-function', '-Wno-unused-variable',
    '-Imain/app', '-Imain/telemetry', '-I'+str(JSON), '-I'+str(MBEDTLS/'include'), '-I'+str(target.parent),
    '-DMBEDTLS_CONFIG_FILE="cloud_sha256_host_config.h"', str(target), str(JSON/'cJSON.c'),
    str(MBEDTLS/'library/sha256.c'), str(MBEDTLS/'library/platform_util.c'), str(MBEDTLS/'library/platform.c'),
    '-o', str(exe)], cwd=ROOT, check=True)
subprocess.run([str(exe)], check=True)
