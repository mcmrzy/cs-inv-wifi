"""Execute the device catalog state machine against real cJSON and radio/OTA spies."""
from pathlib import Path
import json
import subprocess
import sys
root=Path(__file__).resolve().parents[1]
source=(root/'full/panel_firmware_catalog.c').read_text(encoding='utf-8')
for header in ['mqtt_handler.h','esp_timer.h','esp_random.h','freertos/FreeRTOS.h','freertos/semphr.h']:
 source=source.replace(f'#include "{header}"','')
test=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "panel_firmware_catalog.h"
#include "cJSON.h"
#define SN_MAX_LEN 32
#define FIRMWARE_VERSION "0.4.11-modbus-full"
#define pdMS_TO_TICKS(x) (x)
#define pdTRUE 1
typedef int StaticSemaphore_t;
typedef int *SemaphoreHandle_t;
static unsigned clock_time=1000,rng=1,starts,publishes;
static bool held,publish_ok=true,ota_busy;
static char g_device_sn[SN_MAX_LEN]="MB00001122334455",wire[256];
static OtaManifest submitted;
static int64_t esp_timer_get_time(void){return (int64_t)clock_time*1000;}
static unsigned esp_random(void){return rng++;}
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s){return s;}
static int xSemaphoreTake(SemaphoreHandle_t s,unsigned wait){assert(s&&wait==50&&!held);held=true;return 1;}
static void xSemaphoreGive(SemaphoreHandle_t s){assert(s&&held);held=false;}
bool ota_service_is_busy(void){return ota_busy;}
OtaResult ota_service_start_remote(const OtaManifest *m){submitted=*m;starts++;return OTA_RESULT_OK;}
bool full_panel_rpc_publish(const char *p,size_t n){assert(n<sizeof(wire));memcpy(wire,p,n);wire[n]=0;publishes++;return publish_ok;}
SOURCE
static char reply[1900];
static void build(const char *version,const char *url,unsigned total){
 snprintf(reply,sizeof(reply),"{\"v\":1,\"action\":\"catalog\",\"sn\":\"%s\",\"request_id\":\"%s\",\"epoch\":%u,\"index\":%u,\"total\":%u,\"status\":\"ok\",\"firmware\":{\"id\":7,\"target_chip\":\"esp\",\"model\":\"6KU\",\"version\":\"%s\",\"file_url\":\"%s\",\"file_size\":100000,\"file_sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"security_version\":0,\"release_signature\":\"\",\"changelog\":\"Improved screen\"}}",g_device_sn,request_id,epoch,view.index,total,version,url);
}
static void receive(void){assert(panel_firmware_catalog_receive((uint8_t *)reply,strlen(reply),false));}
int main(void){
 panel_firmware_catalog_snapshot_t s;panel_firmware_catalog_init();assert(panel_firmware_catalog_snapshot(&s)&&s.state==PANEL_CATALOG_IDLE);
 assert(!panel_firmware_catalog_refresh());assert(view.state==PANEL_CATALOG_OFFLINE);
 panel_firmware_catalog_network(true);assert(panel_firmware_catalog_refresh()&&view.state==PANEL_CATALOG_LOADING);
 cJSON *sent=cJSON_Parse(wire);assert(sent);cJSON *data=cJSON_GetObjectItem(sent,"data");
 assert(!strcmp(cJSON_GetObjectItem(data,"type")->valuestring,"catalog.query"));cJSON_Delete(sent);
 build("0.5.0-modbus-full","https://firmware.example.com/panel.bin",2);
 assert(panel_firmware_catalog_receive((uint8_t *)reply,strlen(reply),true)&&view.state==PANEL_CATALOG_LOADING);
 char old[1900];strcpy(old,reply);
 char *id=strstr(reply,"request_id\":\"")+13;char c=*id;*id='f';receive();assert(view.state==PANEL_CATALOG_LOADING);*id=c;
 receive();assert(panel_firmware_catalog_snapshot(&s)&&s.state==PANEL_CATALOG_READY&&s.total==2&&s.index==0&&!s.current_version);
 assert(!strcmp(s.version,"0.5.0-modbus-full")&&!strcmp(s.changelog,"Improved screen"));
 assert(panel_firmware_catalog_start()==OTA_RESULT_OK&&starts==1);
 assert(submitted.source==OTA_SOURCE_LOCAL&&submitted.target==OTA_TARGET_ESP&&submitted.timeout_seconds==300);
 assert(!strncmp(submitted.task_id,"panel-",6));assert(panel_firmware_catalog_start()==OTA_RESULT_DUPLICATE&&starts==1);
 assert(panel_firmware_catalog_select(1));strcpy(reply,old);receive();assert(view.state==PANEL_CATALOG_LOADING);
 build(FIRMWARE_VERSION,"https://firmware.example.com/panel.bin",2);receive();assert(view.current_version&&panel_firmware_catalog_start()==OTA_RESULT_INVALID_PARAMS);
 assert(panel_firmware_catalog_refresh());build("0.4.11","https://firmware.example.com/panel.bin",2);receive();assert(view.current_version&&!strcmp(manifest.version,"0.4.11"));
 assert(!panel_firmware_catalog_select(2));
 assert(panel_firmware_catalog_refresh());build("0.6","http://bad.example/panel.bin",1);receive();assert(view.state==PANEL_CATALOG_ERROR&&starts==1);
 assert(panel_firmware_catalog_refresh());build("0.6","https://ok.example/panel.bin",1);strcpy(old,reply);
 panel_firmware_catalog_network(false);panel_firmware_catalog_network(true);assert(panel_firmware_catalog_refresh());strcpy(reply,old);receive();assert(view.state==PANEL_CATALOG_LOADING);
 clock_time+=15000;panel_firmware_catalog_process();assert(view.state==PANEL_CATALOG_TIMEOUT);receive();assert(view.state==PANEL_CATALOG_TIMEOUT);
 assert(panel_firmware_catalog_refresh());build("0.6","https://ok.example/panel.bin",0);receive();assert(view.state==PANEL_CATALOG_EMPTY&&!view.total);
 assert(panel_firmware_catalog_refresh());build("0.6","https://ok.example/panel.bin",1);receive();clock_time+=300001;
 assert(panel_firmware_catalog_start()==OTA_RESULT_TIMEOUT&&view.state==PANEL_CATALOG_TIMEOUT&&starts==1);
 publish_ok=false;assert(!panel_firmware_catalog_refresh()&&view.state==PANEL_CATALOG_ERROR);publish_ok=true;
 CONTRACT_FIXTURE
 ota_busy=true;unsigned before=publishes;assert(!panel_firmware_catalog_refresh()&&publishes==before);ota_busy=false;
 clock_time=UINT32_MAX-200;assert(panel_firmware_catalog_refresh());clock_time=14800;panel_firmware_catalog_process();assert(view.state==PANEL_CATALOG_TIMEOUT);
 assert(!panel_firmware_catalog_receive((uint8_t *)"{\"action\":\"start\"}",18,false));
 assert(!held);puts("PASS firmware catalog: real parser, nonblocking request, epoch/SN/request correlation, retained/stale/HTTP rejection, timeout/wrap, one local OTA start and current-version guard");
}
'''.replace('\nSOURCE\n','\n'+source+'\n')
fixture=root/'tmp/catalog_backend_reply.json'
if fixture.exists():
 contract=json.dumps(fixture.read_text(encoding='utf-8'),ensure_ascii=False)
 test=test.replace('CONTRACT_FIXTURE',r'''
 assert(panel_firmware_catalog_refresh());
 cJSON *contract=cJSON_Parse(CONTRACT_JSON);assert(contract);
 cJSON_ReplaceItemInObject(contract,"request_id",cJSON_CreateString(request_id));
 cJSON_ReplaceItemInObject(contract,"epoch",cJSON_CreateNumber(epoch));
 cJSON_ReplaceItemInObject(contract,"index",cJSON_CreateNumber(view.index));
 assert(cJSON_PrintPreallocated(contract,reply,sizeof(reply),false));cJSON_Delete(contract);
 receive();assert(view.state==PANEL_CATALOG_READY&&view.total==1&&!strcmp(view.version,"0.5.0-modbus-full"));
 puts("PASS firmware catalog server contract: actual Go-published response parsed by production C implementation");
 '''.replace('CONTRACT_JSON',contract))
else:test=test.replace('CONTRACT_FIXTURE','')
path=root/'tmp/firmware_catalog_test.c';path.write_text(test,encoding='utf-8');exe=path.with_suffix('.exe')
sdk=Path('D:/.espressif/v5.5.4/esp-idf/components/json/cJSON')
subprocess.run([sys.argv[1],'-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'full'),'-I'+str(root.parent/'esp32_direct/main/ota'),'-I'+str(sdk),str(path),str(sdk/'cJSON.c'),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
