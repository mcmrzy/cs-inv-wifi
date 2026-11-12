"""Exercise the device-owned forecast, geolocation and city-selection path."""
from pathlib import Path
import re
import subprocess
import sys

root=Path(__file__).resolve().parents[1]
source=re.sub(r'^#include .*\n','',(root/'full/panel_weather.c').read_text(encoding='utf-8-sig'),flags=re.M)
sdk=Path('D:/.espressif/v5.5.4/esp-idf/components/json/cJSON')
test=r'''
#include "panel_weather.h"
#include "cJSON.h"
#include <assert.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_TIMEOUT 17
#define ESP_ERR_HTTP_EAGAIN 18
#define NVS_READWRITE 1
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define pdMS_TO_TICKS(x) (x)
#define pdTRUE 1
#define pdPASS 1
#define HTTP_EVENT_ON_DATA 1
#define ESP_LOGW(tag,...) ((void)(tag),(void)snprintf((char[128]){0},128,__VA_ARGS__))
#define ESP_LOGI ESP_LOGW
typedef int esp_err_t,StaticSemaphore_t,nvs_handle_t;
typedef int *SemaphoreHandle_t;
typedef struct {int event_id,data_len;void *data,*user_data;} esp_http_client_event_t;
typedef struct {
 const char *url;int timeout_ms,buffer_size,buffer_size_tx;
 int (*crt_bundle_attach)(void *);int (*event_handler)(esp_http_client_event_t *);
 void *user_data;bool disable_auto_redirect;
} esp_http_client_config_t;
typedef void *esp_http_client_handle_t;
static uint32_t ms=1000;
static const time_t day=1793923200;
static bool wifi,cloud,admin,ble,handshake,timeout,oversized,invalid_daily,save_error;
static unsigned free_heap=45728, largest_block=18432, pauses,resumes,geo_calls,forecast_calls,search_calls,tasks;
static void (*task_fn)(void *);static void *task_arg;
static esp_http_client_config_t http;
static char blob[128];static size_t blob_len;
static int64_t esp_timer_get_time(void){return (int64_t)ms*1000;}
static time_t test_time(time_t *out){if(out)*out=day+40000;return day+40000;}
#define time test_time
static struct tm *fixture_gmtime(const time_t *t,struct tm *out){struct tm *p=gmtime(t);if(!p)return NULL;*out=*p;return out;}
#define gmtime_r fixture_gmtime
static bool wifi_sta_is_connected(void){return wifi;}
static bool full_mqtt_client_resident(void){return cloud;}
static bool mqtt_is_connected(void){return cloud;}
static bool mqtt_is_connecting(void){return handshake;}
static bool ble_prov_is_connected(void){return ble;}
static bool ota_service_admin_begin(void){if(admin)return false;admin=true;return true;}
static void ota_service_admin_end(void){assert(admin);admin=false;}
static bool mqtt_pause_for_local(void){assert(cloud&&admin);cloud=false;pauses++;return true;}
static bool mqtt_resume_after_local(void){assert(!cloud&&admin);cloud=true;resumes++;return true;}
static unsigned heap_caps_get_free_size(int caps){(void)caps;return free_heap;}
static unsigned heap_caps_get_largest_free_block(int caps){(void)caps;return largest_block;}
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *p){return p;}
static int xSemaphoreTake(SemaphoreHandle_t p,unsigned timeout_ms){(void)timeout_ms;assert(!*p);*p=1;return pdTRUE;}
static void xSemaphoreGive(SemaphoreHandle_t p){assert(*p);*p=0;}
static int xTaskCreate(void (*f)(void *),const char *name,unsigned stack,void *arg,unsigned priority,void *out){
 assert(!strcmp(name,"panel_weather")&&stack==8192&&priority==4&&!out&&!task_fn);
 task_fn=f;task_arg=arg;tasks++;return pdPASS;
}
static void vTaskDelete(void *task){assert(!task);}
static int nvs_open(const char *name,int mode,nvs_handle_t *handle){assert(!strcmp(name,"panel_weather")&&mode==1);*handle=1;return ESP_OK;}
static int nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *size){
 assert(h==1&&!strcmp(key,"city"));if(!blob_len||*size<blob_len)return ESP_FAIL;
 memcpy(out,blob,blob_len);*size=blob_len;return ESP_OK;
}
static int nvs_set_blob(nvs_handle_t h,const char *key,const void *data,size_t size){
 assert(h==1&&!strcmp(key,"city")&&size<=sizeof(blob));if(save_error)return ESP_FAIL;
 memcpy(blob,data,size);blob_len=size;return ESP_OK;
}
static int nvs_commit(nvs_handle_t h){assert(h==1);return ESP_OK;}
static int esp_crt_bundle_attach(void *cfg){(void)cfg;return 0;}
static esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config){
 http=*config;assert(!strncmp(http.url,"https://",8)&&http.crt_bundle_attach==esp_crt_bundle_attach&&http.disable_auto_redirect);return &http;
}
static int esp_http_client_get_status_code(esp_http_client_handle_t h){assert(h==&http);return 200;}
static int esp_http_client_cleanup(esp_http_client_handle_t h){assert(h==&http);return ESP_OK;}
static int esp_http_client_perform(esp_http_client_handle_t h){
 assert(h==&http && wifi && admin);
 char body[600];
 if(strstr(http.url,"ipwho.is")){
  geo_calls++;strcpy(body,"{\"success\":true,\"city\":\"Shanghai\",\"latitude\":31.2,\"longitude\":121.4}");
 }else if(strstr(http.url,"/search")){
  search_calls++;assert(strstr(http.url,"name=Bei%20Jing"));
  strcpy(body,"{\"results\":[{\"name\":\"Beijing\",\"country_code\":\"CN\",\"latitude\":39.9,\"longitude\":116.4}]}");
 }else{
  forecast_calls++;
  assert(strstr(http.url,"forecast_days=7")&&strstr(http.url,"timeformat=unixtime"));
  assert(strstr(http.url,forecast_calls<3?"latitude=31.200000":"latitude=39.900000"));
  if(timeout)return ESP_ERR_TIMEOUT;
  snprintf(body,sizeof(body),"{\"utc_offset_seconds\":28800,\"current\":{\"time\":%lld,\"temperature_2m\":20.2,\"weather_code\":2},"
    "\"daily\":{\"time\":[%lld,%lld,%lld,%lld,%lld,%lld,%lld],"
    "\"temperature_2m_min\":[1,1,1,1,1,1,1],\"temperature_2m_max\":[20,20,20,20,20,20,%d],\"weather_code\":[0,1,2,3,45,61,95]}}",
    (long long)day+40000,(long long)day-28800,(long long)day+57600,(long long)day+144000,
    (long long)day+230400,(long long)day+316800,(long long)day+403200,(long long)day+489600,invalid_daily?-2:20);
 }
 esp_http_client_event_t event={HTTP_EVENT_ON_DATA,oversized?5000:(int)strlen(body),body,http.user_data};
 return http.event_handler(&event);
}
SOURCE
static void execute(void){
 assert(!task_fn&&panel_weather_network_busy());
 ms+=249;panel_weather_process(false);assert(!task_fn);
 ms++;panel_weather_process(false);
 assert(task_fn&&panel_weather_network_busy());void(*run)(void *)=task_fn;task_fn=NULL;run(task_arg);
 ms+=249;panel_weather_process(false);assert(panel_weather_network_busy());
 ms++;panel_weather_process(false);assert(!panel_weather_network_busy()&&!admin);
}
int main(void){
 panel_weather_init();panel_weather_process(false);assert(!tasks);
 wifi=true;ble=true;panel_weather_process(false);assert(!tasks);
 ble=false;panel_weather_process(false);execute();
 panel_weather_snapshot_t out;assert(panel_weather_snapshot(&out)&&out.valid&&!out.stale&&out.days==7);
 assert(!strcmp(out.city,"Shanghai")&&out.temperature_10==202&&geo_calls==1&&forecast_calls==1&&!cloud);
 timeout=true;cloud=true;panel_weather_refresh();panel_weather_process(false);execute();
 assert(!pauses&&!resumes&&cloud);
 assert(panel_weather_snapshot(&out)&&out.stale&&out.state==PANEL_WEATHER_TIMEOUT&&out.days==7);
 assert(panel_weather_search("Bei Jing"));panel_weather_process(false);execute();
 char city[32];assert(search_calls==1&&panel_weather_city(0,city,sizeof(city))&&!strcmp(city,"Beijing, CN"));
 save_error=true;assert(!panel_weather_set_city(0));save_error=false;assert(panel_weather_set_city(0));
 timeout=false;panel_weather_process(false);execute();
 assert(panel_weather_snapshot(&out)&&out.valid&&out.configured_city&&!strcmp(out.city,"Beijing")&&geo_calls==1);
 mutex=NULL;panel_weather_init();assert(!strcmp(saved_city.city,"Beijing"));
 invalid_daily=true;panel_weather_refresh();panel_weather_process(false);execute();
 assert(panel_weather_snapshot(&out)&&out.valid&&out.stale&&out.state==PANEL_WEATHER_ERROR);
 free_heap=36000;largest_block=8704;
 unsigned before=tasks;
 panel_weather_refresh();panel_weather_process(false);
 assert(!task_fn&&tasks==before&&!admin&&cloud&&!pauses&&!resumes);
 wifi=false;panel_weather_process(false);assert(panel_weather_snapshot(&out)&&out.stale&&out.state==PANEL_WEATHER_OFFLINE);
 assert(panel_weather_set_city(-1)&&!saved_city.city[0]);
 puts("PASS weather: direct HTTPS without MQTT, automatic IP, city search and persistence, seven-day parsing, stale fallback and serialized network recovery");
}
'''.replace('SOURCE',source)
folder=root/'tmp/weather'
folder.mkdir(parents=True,exist_ok=True)
path=folder/'weather_test.c'
path.write_text(test,encoding='utf-8')
exe=path.with_suffix('.exe')
subprocess.run([sys.argv[1],'-std=c11','-Os','-Wall','-Wextra','-Werror','-I'+str(root/'full'),
                '-I'+str(sdk),str(path),str(sdk/'cJSON.c'),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
