"""Exercise the actual /info formatter and one-time model registration replay."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
shared = root.parent / 'esp32_direct'
source = (shared / 'main/telemetry/telemetry.c').read_text(encoding='utf-8')
functions = source[source.index('static void publish_info('):source.index('/* ---- config (V2.1')]
test = r'''
#include <assert.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include "arm_param.h"
#include "factory_json.h"
#define PANEL_FULL_DIRECT 1
#define ESP_LOGW(...) ((void)0)
#define TOPIC_INFO "info"
#define FIRMWARE_VERSION "0.4.1-modbus-full"
#define ESP_MAC_WIFI_STA 0
static char g_device_sn[]="MB00001122334455";
static unsigned clock_ms,revision,published;
static bool provisioned;
static const char *detected_model;
static char payload[2048];
static atomic_uint s_info_revision_sent,s_info_revision_attempted,s_info_last_attempt;
static atomic_bool s_info_pending,s_info_model_repeat,s_info_model_enabled;
static _Atomic(const char *) s_info_model_sent;
static uint32_t s_last_model_probe;
static uint32_t now_ms(void){return clock_ms;}
static uint32_t factory_identity_revision(void){return revision;}
static uint32_t factory_identity_runtime_generation(void){return 0;}
static factory_result_t factory_identity_read(factory_record_t *r){(void)r;return FACTORY_UNPROVISIONED;}
static bool factory_identity_admin_sn(char *out,size_t n){snprintf(out,n,"%s",g_device_sn);return provisioned;}
static const char *full_modbus_registry_model(void){return detected_model;}
static int esp_read_mac(uint8_t *out,int kind){(void)kind;memset(out,2,6);return 0;}
static bool mqtt_is_connected(void){return true;}
static int mqtt_publish_volatile(const char *topic,const char *json,int length,int qos){
 assert(!strcmp(topic,"info") && length==0 && qos==1);
 assert(strlen(json)<sizeof(payload));strcpy(payload,json);published++;return 0;
}
FUNCTIONS
int main(void){
 static const char code[]="PWR4KL24H1";
 detected_model=code;
 publish_info(NULL);assert(published==1 && !strstr(payload,"\"model\":\"PWR4KL24H1\""));
 clock_ms=2000;publish_identity_changes();assert(published==1); /* Development identity cannot bind. */
 provisioned=true;revision=1;detected_model=NULL;
 publish_identity_changes();assert(published==2);
 detected_model=code;clock_ms=3500;publish_identity_changes();assert(published==3);
 cJSON *info=cJSON_Parse(payload);assert(info);
 assert(!strcmp(cJSON_GetObjectItem(info,"model")->valuestring,code));
 assert(!strcmp(cJSON_GetObjectItem(info,"device_type")->valuestring,"inverter"));
 assert(!strcmp(cJSON_GetObjectItem(info,"sn")->valuestring,g_device_sn));
 assert(!strcmp(cJSON_GetObjectItem(info,"firmware_esp")->valuestring,FIRMWARE_VERSION));
 assert(!cJSON_GetObjectItem(info,"rated_power") && !cJSON_GetObjectItem(info,"firmware_dsp"));
 cJSON_Delete(info);
 char first[sizeof(payload)];strcpy(first,payload);
 clock_ms=5000;publish_identity_changes();assert(published==4 && !strcmp(first,payload));
 clock_ms=10000;publish_identity_changes();assert(published==4);
 puts("PASS /info: actual SN, delayed verified model, one identical replay, omitted unknown fields");
}
'''.replace('FUNCTIONS', functions)
cjson = Path('D:/.espressif/v5.5.4/esp-idf/components/json/cJSON')
target = root / 'tmp/registry_info_test.c'
target.write_text(test, encoding='utf-8')
exe = target.with_suffix('.exe')
subprocess.run([sys.argv[1], '-std=c11', '-Wall', '-Wextra', '-Werror',
    '-I'+str(shared/'main/telemetry'), '-I'+str(shared/'main/factory'), '-I'+str(cjson),
    str(target), str(root/'full/factory_json.c'), str(shared/'main/factory/factory_record.c'),
    str(cjson/'cJSON.c'), '-lm', '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True)
