"""Exercise the device-owned forecast, geolocation and city-selection path."""
from pathlib import Path
import re
import subprocess
import sys

root=Path(__file__).resolve().parents[1]
source=re.sub(r'^#include .*\n','',(root/'full/panel_weather.c').read_text(encoding='utf-8-sig'),flags=re.M)
app_source=(root/'full/app_main.c').read_text(encoding='utf-8')
indicator_tick=app_source[app_source.index('static void indicators_tick(void)'):
                          app_source.index('static uint32_t now_ms(void)')]
sdk=Path('D:/.espressif/v5.5.4/esp-idf/components/json/cJSON')
test=r'''
#include "panel_weather.h"
#include "panel_indicators.h"
#include "cJSON.h"
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_HTTP_CONNECT 0x7002
#define ESP_ERR_TIMEOUT 17
#define ESP_ERR_HTTP_EAGAIN 18
#define ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME 0x8001
#define ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT 0x8006
#define ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED 0x801a
#define MBEDTLS_ERR_SSL_TIMEOUT -0x6800
#define NVS_READWRITE 1
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define pdMS_TO_TICKS(x) (x)
#define pdTRUE 1
#define pdPASS 1
#define HTTP_EVENT_ON_DATA 1
#define HTTP_EVENT_ON_CONNECTED 2
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
static bool resident,paused,pause_blocked,geo_timeout,deny_next_lock,reject_lock_after_pause;
static bool ble_ready,ble_paused,ble_pause_fails;
static unsigned ble_pauses,ble_resumes,ble_resume_failures;
static void *ble_reserve;
static unsigned ble_reserve_bytes;
static bool reserve_fails,fragmented_ble_heap;
static unsigned released_free=45728,released_block=18432,resume_failures;
static const char *expected_latitude="latitude=31.200000";
static unsigned free_heap=45728, largest_block=18432, pauses,resumes,geo_calls,forecast_calls,search_calls,tasks;
static void (*task_fn)(void *);static void *task_arg;
static esp_http_client_config_t http;
static bool http_live;
static bool http_init_error,malformed;
static int http_status=200;
static int tls_error,tls_code,socket_error;
static char blob[128];static size_t blob_len;
static int64_t esp_timer_get_time(void){return (int64_t)ms*1000;}
static time_t test_time(time_t *out){if(out)*out=day+40000;return day+40000;}
#define time test_time
static struct tm *fixture_gmtime(const time_t *t,struct tm *out){struct tm *p=gmtime(t);if(!p)return NULL;*out=*p;return out;}
#define gmtime_r fixture_gmtime
static bool wifi_sta_is_connected(void){return wifi;}
static bool full_mqtt_client_resident(void){return resident||cloud;}
static bool mqtt_is_connected(void){return cloud;}
static bool mqtt_is_connecting(void){return handshake;}
static bool ble_prov_is_connected(void){return ble;}
static int ble_prov_weather_restore_block(void){return 8192;}
static inline bool ble_prov_is_ready(void){return ble_ready&&!ble_paused;}
static inline bool ble_prov_suspend_for_weather(void){
 assert(admin&&!ble&&ble_ready&&!ble_paused&&!cloud);
 ble_pauses++;ble_paused=true;return !ble_pause_fails;
}
static inline bool ble_prov_restore_after_weather(void){
 assert(admin&&ble_paused&&!ble_reserve);ble_resumes++;
 if(fragmented_ble_heap)return false;
 if(ble_resume_failures){ble_resume_failures--;return false;}
 ble_paused=false;return true;
}
static bool ota_service_admin_begin(void){if(admin)return false;admin=true;return true;}
static void ota_service_admin_end(void){assert(admin);admin=false;}
static bool mqtt_pause_for_local(void){
 assert((resident||cloud)&&admin);paused=true;pauses++;
 if(pause_blocked)return false;
 cloud=false;resident=false;deny_next_lock=reject_lock_after_pause;reject_lock_after_pause=false;return true;
}
static bool mqtt_resume_after_local(void){
 assert(paused&&admin);resumes++;
 if(resume_failures){resume_failures--;return false;}
 paused=false;cloud=true;resident=true;return true;
}
static unsigned heap_caps_get_free_size(int caps){(void)caps;return ble_paused?82212-ble_reserve_bytes:paused&&!cloud?released_free:free_heap;}
static unsigned heap_caps_get_largest_free_block(int caps){(void)caps;return ble_paused?(fragmented_ble_heap?7168:ble_reserve?14336:22528):paused&&!cloud?released_block:largest_block;}
static inline void *heap_caps_malloc(size_t size,int caps){
 assert(caps==3&&ble_paused&&!ble_reserve&&size==8192);
 if(reserve_fails)return NULL;
 ble_reserve=malloc(size);if(ble_reserve)ble_reserve_bytes=(unsigned)size;
 return ble_reserve;
}
static inline void heap_caps_free(void *ptr){
 assert(ptr==ble_reserve);free(ptr);ble_reserve=NULL;ble_reserve_bytes=0;
}
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *p){return p;}
static int xSemaphoreTake(SemaphoreHandle_t p,unsigned timeout_ms){
 (void)timeout_ms;
 if(deny_next_lock){deny_next_lock=false;return 0;}
 assert(!*p);*p=1;return pdTRUE;
}
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
 assert(!http_live);if(http_init_error)return NULL;http_live=true;
 http=*config;assert(!strncmp(http.url,"https://",8)&&http.crt_bundle_attach==esp_crt_bundle_attach&&http.disable_auto_redirect);return &http;
}
static int esp_http_client_get_status_code(esp_http_client_handle_t h){assert(h==&http);return http_status;}
static inline int esp_http_client_get_errno(esp_http_client_handle_t h){assert(h==&http&&http_live);return socket_error;}
static inline int esp_http_client_get_and_clear_last_tls_error(esp_http_client_handle_t h,int *code,int *flags){
 assert(h==&http&&http_live);*code=tls_code;*flags=0;return tls_error;
}
static int esp_http_client_cleanup(esp_http_client_handle_t h){assert(h==&http&&http_live);http_live=false;return ESP_OK;}
static cJSON *parse_after_http_cleanup(const char *body,size_t length,const char **end,int terminated){
 assert(!http_live && "release HTTP/TLS memory before allocating the JSON tree");
 return cJSON_ParseWithLengthOpts(body,length,end,terminated);
}
#define cJSON_ParseWithLengthOpts parse_after_http_cleanup
static int esp_http_client_perform(esp_http_client_handle_t h){
 assert(h==&http && wifi && admin);
 char body[600];
 if(strstr(http.url,"ipwho.is")){
  geo_calls++;if(geo_timeout)return ESP_ERR_HTTP_CONNECT;strcpy(body,"{\"success\":true,\"city\":\"Shanghai\",\"latitude\":31.2,\"longitude\":121.4}");
 }else if(strstr(http.url,"/search")){
  search_calls++;assert(strstr(http.url,"name=Bei%20Jing"));
  strcpy(body,"{\"results\":[{\"name\":\"Beijing\",\"country_code\":\"CN\",\"latitude\":39.9,\"longitude\":116.4}]}");
 }else{
  forecast_calls++;
  assert(strstr(http.url,"forecast_days=7")&&strstr(http.url,"timeformat=unixtime"));
  assert(strstr(http.url,expected_latitude));
  if(tls_error || socket_error)return ESP_ERR_HTTP_CONNECT;
  if(timeout)return ESP_ERR_TIMEOUT;
  snprintf(body,sizeof(body),"{\"utc_offset_seconds\":28800,\"current\":{\"time\":%lld,\"temperature_2m\":20.2,\"weather_code\":2},"
    "\"daily\":{\"time\":[%lld,%lld,%lld,%lld,%lld,%lld,%lld],"
    "\"temperature_2m_min\":[1,1,1,1,1,1,1],\"temperature_2m_max\":[20,20,20,20,20,20,%d],\"weather_code\":[0,1,2,3,45,61,95]}}",
    (long long)day+40000,(long long)day-28800,(long long)day+57600,(long long)day+144000,
    (long long)day+230400,(long long)day+316800,(long long)day+403200,(long long)day+489600,invalid_daily?-2:20);
 }
 if(malformed)strcpy(body,"{invalid");
 esp_http_client_event_t connected={HTTP_EVENT_ON_CONNECTED,0,NULL,http.user_data};
 assert(http.event_handler(&connected)==ESP_OK);
 size_t length=strlen(body);
 esp_http_client_event_t event={HTTP_EVENT_ON_DATA,oversized?5000:24,body,http.user_data};
 if(oversized)return http.event_handler(&event);
 assert(length>=24 || malformed);
 if(length<24)event.data_len=(int)length;
 int err=http.event_handler(&event);
 if(err!=ESP_OK || length==(size_t)event.data_len)return err;
 event.data=body+event.data_len;
 event.data_len=(int)length-event.data_len;
 return http.event_handler(&event);
}
static size_t peak_response_alloc;
static inline void *response_alloc(void *current,size_t size){
 if(size>peak_response_alloc)peak_response_alloc=size;
 return realloc(current,size);
}
#define malloc(size) response_alloc(NULL,size)
#define realloc response_alloc
SOURCE
#undef realloc
#undef malloc
static void execute(void){
 assert(!task_fn&&panel_weather_network_busy());
 ms+=249;panel_weather_process(false);assert(!task_fn);
 ms++;panel_weather_process(false);
 assert(task_fn&&panel_weather_network_busy());void(*run)(void *)=task_fn;task_fn=NULL;run(task_arg);
 ms+=249;panel_weather_process(false);assert(panel_weather_network_busy());
 ms++;panel_weather_process(false);assert(!panel_weather_network_busy()&&!admin);
}
/* execute() returns 250 ms after the worker scheduled its next request. */
static void retry_after(unsigned delay){
 unsigned before=tasks,pause_count=pauses;
 ms+=delay-251;panel_weather_process(false);
 assert(!panel_weather_network_busy()&&!admin&&cloud&&tasks==before&&pauses==pause_count);
 ms++;panel_weather_process(false);
 assert(panel_weather_network_busy());execute();
 assert(tasks==before+1&&!http_live);
}
#define WIFI_DEFAULT_SSID ""
static atomic_bool s_prov_busy;
static panel_led_network_t observed_network;
static bool storage_has_wifi_config(void){return true;}
static bool ble_prov_is_suspended(void){return ble_paused;}
static bool mqtt_is_paused_for_local(void){return paused;}
static bool full_alarm_needs_attention(void){return false;}
static void full_indicators_tick(const panel_led_network_t *net){observed_network=*net;}
INDICATOR_TICK

static void weather_net_indicator(void){
 ms=1000;wifi=true;cloud=true;paused=false;indicators_tick();
 assert(observed_network.cloud && !observed_network.reconnecting);
 cloud=false;paused=true;atomic_store(&network_owned,true);indicators_tick();
 assert(observed_network.wifi && !observed_network.cloud && !observed_network.reconnecting);
 ms+=30000;indicators_tick();assert(!observed_network.reconnecting);
 paused=false;atomic_store(&network_owned,false);indicators_tick();
 ms+=14999;indicators_tick();assert(!observed_network.reconnecting);
 ms++;indicators_tick();assert(observed_network.reconnecting);
 paused=true;atomic_store(&network_owned,true);indicators_tick();assert(observed_network.reconnecting);
 paused=false;atomic_store(&network_owned,false);
 cloud=true;indicators_tick();assert(observed_network.cloud && !observed_network.reconnecting);
 cloud=false;paused=true;atomic_store(&network_owned,true);indicators_tick();assert(!observed_network.reconnecting);
 wifi=false;indicators_tick();assert(observed_network.reconnecting);
 paused=false;atomic_store(&network_owned,false);
 puts("PASS weather NET indicator: pause and recovery window, existing cloud failure, Wi-Fi loss");
}
int main(void){
 panel_weather_init();panel_weather_process(false);assert(!tasks);
 wifi=true;ble=true;panel_weather_process(false);assert(!tasks);
 ble=false;panel_weather_process(false);execute();
 panel_weather_snapshot_t out;assert(panel_weather_snapshot(&out)&&out.valid&&!out.stale&&out.days==7);
 assert(peak_response_alloc>0&&peak_response_alloc<=1024);
 assert(!strcmp(out.city,"Shanghai")&&out.temperature_10==202&&geo_calls==1&&forecast_calls==1&&!cloud);
 timeout=true;cloud=true;panel_weather_refresh();panel_weather_process(false);execute();
 assert(!pauses&&!resumes&&cloud);
 assert(panel_weather_snapshot(&out)&&out.stale&&out.state==PANEL_WEATHER_TIMEOUT&&out.days==7);
 assert(panel_weather_search("Bei Jing"));panel_weather_process(false);execute();
 char city[32];assert(search_calls==1&&panel_weather_city(0,city,sizeof(city))&&!strcmp(city,"Beijing, CN"));
 save_error=true;assert(!panel_weather_set_city(0));save_error=false;assert(panel_weather_set_city(0));
 expected_latitude="latitude=39.900000";
 timeout=false;panel_weather_process(false);execute();
 assert(panel_weather_snapshot(&out)&&out.valid&&out.configured_city&&!strcmp(out.city,"Beijing")&&geo_calls==1);
 mutex=NULL;panel_weather_init();assert(!strcmp(saved_city.city,"Beijing"));
 invalid_daily=true;panel_weather_refresh();panel_weather_process(false);execute();
 assert(panel_weather_snapshot(&out)&&out.valid&&out.stale&&out.state==PANEL_WEATHER_ERROR);
 /* Reproduce the field log after MQTT connects: free=16604, block=7680. */
 invalid_daily=false;free_heap=16604;largest_block=7680;
 unsigned before=tasks;
 panel_weather_refresh();ble=true;panel_weather_process(false);
 assert(!panel_weather_network_busy()&&!pauses&&cloud);
 ble=false;panel_weather_process(true);
 assert(!panel_weather_network_busy()&&!pauses&&cloud);
 panel_weather_process(false);
 assert(panel_weather_network_busy()&&paused&&!cloud&&pauses==1);
 execute();
 assert(tasks==before+1&&!paused&&cloud&&resumes==1);
 assert(panel_weather_snapshot(&out)&&out.valid&&!out.stale&&out.state==PANEL_WEATHER_READY);

 /* A failed request must return MQTT ownership and keep the previous forecast. */
 timeout=true;panel_weather_refresh();panel_weather_process(false);execute();
 assert(!paused&&cloud&&pauses==2&&resumes==2);
 assert(panel_weather_snapshot(&out)&&out.valid&&out.stale&&out.state==PANEL_WEATHER_TIMEOUT);
 timeout=false;

 /* The shared pause API can reject while its cache worker drains. */
 pause_blocked=true;panel_weather_refresh();panel_weather_process(false);
 assert(!panel_weather_network_busy()&&!paused&&!admin&&cloud&&pauses==3&&resumes==3);
 pause_blocked=false;panel_weather_process(false);execute();
 assert(!paused&&cloud&&pauses==4&&resumes==4);

 /* Never start TLS if releasing MQTT still leaves insufficient memory. */
 released_free=16604;released_block=7680;before=tasks;
 panel_weather_refresh();panel_weather_process(false);
 assert(panel_weather_network_busy()&&paused&&!cloud);
 ms+=1500;panel_weather_process(false);
 assert(!task_fn&&tasks==before&&panel_weather_network_busy());
 assert(panel_weather_snapshot(&out)&&out.state==PANEL_WEATHER_ERROR);
 ms+=250;panel_weather_process(false);
 assert(!panel_weather_network_busy()&&!paused&&!admin&&cloud&&pauses==5&&resumes==5);
 released_free=45728;released_block=18432;

 /* Retain network ownership until a failed MQTT recreation can be retried. */
 panel_weather_refresh();panel_weather_process(false);
 ms+=250;panel_weather_process(false);assert(task_fn);
 void(*run)(void *)=task_fn;task_fn=NULL;run(task_arg);
 resume_failures=1;ms+=250;panel_weather_process(false);
 assert(panel_weather_network_busy()&&paused&&admin&&!cloud&&resumes==6);
 ms+=250;panel_weather_process(false);
 assert(!panel_weather_network_busy()&&!paused&&!admin&&cloud&&pauses==6&&resumes==7);

 /* A geolocation connection timeout must also restore the cloud connection. */
 wifi=false;panel_weather_process(false);assert(panel_weather_snapshot(&out)&&out.stale&&out.state==PANEL_WEATHER_OFFLINE);
 assert(panel_weather_set_city(-1)&&!saved_city.city[0]);
 expected_latitude="latitude=31.200000";
 wifi=true;geo_timeout=true;tls_error=ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT;
 panel_weather_process(false);execute();
 assert(panel_weather_snapshot(&out)&&!out.valid&&out.state==PANEL_WEATHER_TIMEOUT);
 assert(!paused&&cloud&&pauses==7&&resumes==8);
 geo_timeout=false;tls_error=0;panel_weather_refresh();panel_weather_process(false);execute();
 assert(panel_weather_snapshot(&out)&&out.valid&&!out.stale&&out.state==PANEL_WEATHER_READY);
 assert(!paused&&cloud&&pauses==8&&resumes==9);

 /* Release a disconnected resident client before allocating weather TLS. */
 free_heap=45728;largest_block=18432;cloud=false;resident=true;
 panel_weather_refresh();panel_weather_process(false);execute();
 assert(!paused&&cloud&&pauses==9&&resumes==10);
 /* Losing the weather lock after releasing MQTT must still finish restoration. */
 free_heap=16604;largest_block=7680;before=tasks;
 reject_lock_after_pause=true;resume_failures=1;
 panel_weather_refresh();panel_weather_process(false);
 assert(panel_weather_network_busy()&&paused&&admin&&!cloud&&tasks==before);
 ms+=250;panel_weather_process(false);
 assert(panel_weather_network_busy()&&paused&&admin&&!cloud);
 ms+=250;panel_weather_process(false);
 assert(!panel_weather_network_busy()&&!paused&&!admin&&cloud&&tasks==before);
 /* Consecutive automatic failures must leave MQTT online for longer intervals. */
 timeout=true;panel_weather_refresh();panel_weather_process(false);execute();
 const unsigned delays[]={60000,120000,240000,480000,900000,900000};
 for(unsigned i=0;i<sizeof(delays)/sizeof(delays[0]);i++)retry_after(delays[i]);
 /* A manual refresh bypasses and resets the automatic failure backoff. */
 panel_weather_refresh();panel_weather_process(false);execute();retry_after(60000);
 /* An automatic success restores the regular cadence and resets failure history. */
 timeout=false;retry_after(120000);
 assert(panel_weather_snapshot(&out)&&out.state==PANEL_WEATHER_READY&&!out.stale);
 timeout=true;retry_after(1800000);retry_after(60000);
 /* Reconnecting WiFi starts a fresh attempt instead of inheriting old failures. */
 wifi=false;panel_weather_process(false);wifi=true;panel_weather_process(false);execute();
 retry_after(60000);
 /* Deadlines keep working when the 32-bit millisecond clock wraps. */
 ms=UINT32_MAX-30000;panel_weather_refresh();panel_weather_process(false);execute();
 retry_after(60000);
 /* All HTTP error exits must release resources and retain the last good forecast. */
 timeout=false;
 for(unsigned mode=0;mode<4;mode++){
  oversized=mode==0;malformed=mode==1;http_status=mode==2?503:200;http_init_error=mode==3;
  panel_weather_refresh();panel_weather_process(false);execute();
  assert(!http_live&&cloud&&!paused);
  assert(panel_weather_snapshot(&out)&&out.valid&&out.stale&&out.state==PANEL_WEATHER_ERROR);
 }
 oversized=false;malformed=false;http_status=200;http_init_error=false;
 /* The same HTTP connection error can come from DNS, TLS, or a real timeout. */
 const struct {int tls,mbedtls,system;panel_weather_state_t state;} connection_failures[]={
  {ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME,0,0,PANEL_WEATHER_ERROR},
  {ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED,0x2700,0,PANEL_WEATHER_ERROR},
  {0,0,ECONNREFUSED,PANEL_WEATHER_ERROR},
  {ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED,-MBEDTLS_ERR_SSL_TIMEOUT,0,PANEL_WEATHER_TIMEOUT},
  {0,0,ETIMEDOUT,PANEL_WEATHER_TIMEOUT},
 };
 for(unsigned i=0;i<sizeof(connection_failures)/sizeof(connection_failures[0]);i++){
  tls_error=connection_failures[i].tls;tls_code=connection_failures[i].mbedtls;
  socket_error=connection_failures[i].system;
  panel_weather_refresh();panel_weather_process(false);execute();
  assert(!http_live&&cloud&&!paused);
  assert(panel_weather_snapshot(&out)&&out.valid&&out.stale&&out.state==connection_failures[i].state);
 }
 tls_error=0;tls_code=0;socket_error=0;
 /* Admission failures back off too, without starting a TLS worker. */
 released_free=16604;released_block=7680;before=tasks;
 panel_weather_refresh();panel_weather_process(false);
 ms+=1500;panel_weather_process(false);ms+=250;panel_weather_process(false);
 for(unsigned delay=60000;delay<=120000;delay*=2){
  unsigned pause_count=pauses;
  ms+=delay-251;panel_weather_process(false);
  assert(!panel_weather_network_busy()&&cloud&&pauses==pause_count&&tasks==before);
  ms++;panel_weather_process(false);assert(panel_weather_network_busy());
  if(delay==120000){released_free=45728;released_block=18432;execute();}
  else{ms+=1500;panel_weather_process(false);ms+=250;panel_weather_process(false);}
 }
 assert(tasks==before+1&&cloud&&!paused);
 assert(panel_weather_snapshot(&out)&&out.state==PANEL_WEATHER_READY);
 /* Field retry: enough total heap after MQTT release, but only a 9216-byte block. */
 ble_ready=true;released_free=36636;released_block=9216;before=tasks;
 panel_weather_refresh();panel_weather_process(false);
 ms+=1499;panel_weather_process(false);assert(!task_fn&&!ble_paused&&paused);
 ms++;panel_weather_process(false);assert(ble_paused&&!task_fn&&ble_pauses==1&&admin);
 ms+=250;panel_weather_process(false);assert(task_fn&&ble_paused);
 assert(ble_reserve_bytes==8192);
 run=task_fn;task_fn=NULL;run(task_arg);
 assert(tasks==before+1&&panel_weather_snapshot(&out)&&out.state==PANEL_WEATHER_READY);
 ble_resume_failures=1;ms+=250;panel_weather_process(false);
 assert(ble_paused&&ble_resumes==1&&paused&&!cloud&&admin);
 ms+=250;panel_weather_process(false);
 assert(!ble_paused&&ble_resumes==2&&paused&&!cloud&&admin);
 ms+=250;panel_weather_process(false);
 assert(!paused&&cloud&&!admin&&!panel_weather_network_busy());
 /* A racing BLE connection vetoes teardown even after the MQTT client is gone. */
 panel_weather_refresh();panel_weather_process(false);
 ble=true;ms+=1500;panel_weather_process(false);
 ms+=250;panel_weather_process(false);
 assert(!task_fn&&ble_pauses==1&&ble_resumes==2&&cloud&&!admin);
 ble=false;
 /* A failed pause can need rollback and must never start HTTPS. */
 ble_pause_fails=true;panel_weather_refresh();panel_weather_process(false);
 ms+=1500;panel_weather_process(false);
 assert(!task_fn&&ble_paused&&ble_pauses==2&&paused&&admin);
 ms+=250;panel_weather_process(false);
 ms+=250;panel_weather_process(false);
 assert(!ble_paused&&ble_resumes==3&&!paused&&cloud&&!admin);
 /* Refuse a weather allocation that cannot protect the BLE restore block. */
 ble_pause_fails=false;reserve_fails=true;before=tasks;
 panel_weather_refresh();panel_weather_process(false);
 ms+=1500;panel_weather_process(false);ms+=250;panel_weather_process(false);
 assert(!task_fn&&tasks==before&&!ble_reserve);
 ms+=250;panel_weather_process(false);ms+=250;panel_weather_process(false);
 assert(!ble_paused&&!paused&&cloud&&!admin&&!panel_weather_network_busy());
 reserve_fails=false;
 /* Persistent BLE failures must return MQTT ownership and defer BLE recovery. */
 panel_weather_refresh();panel_weather_process(false);
 ms+=1500;panel_weather_process(false);ms+=250;panel_weather_process(false);
 assert(task_fn&&ble_reserve_bytes==8192);
 run=task_fn;task_fn=NULL;run(task_arg);
 fragmented_ble_heap=true;
 for(unsigned i=0;i<4;i++){ms+=250;panel_weather_process(false);}
 assert(!ble_reserve&&ble_paused&&!paused&&cloud&&!admin&&!panel_weather_network_busy());
 before=ble_resumes;ms+=30000;panel_weather_process(false);
 assert(ble_resumes==before&&cloud&&!admin&&!panel_weather_network_busy());
 wifi=false;panel_weather_process(false);
 assert(panel_weather_snapshot(&out)&&out.valid&&out.stale&&out.state==PANEL_WEATHER_OFFLINE);
 wifi=true;panel_weather_process(false);
 /* Background recovery must respect OTA and then restore BLE when memory returns. */
 fragmented_ble_heap=false;admin=true;panel_weather_process(false);
 assert(ble_resumes==before&&cloud&&admin);admin=false;
 panel_weather_process(false);
 assert(!ble_paused&&ble_resumes==before+1&&cloud&&!admin&&!panel_weather_network_busy());
 weather_net_indicator();
 puts("PASS weather cleanup: 82212/22528 field heap, protected profile-sized BLE block, reservation failure, bounded restore retries, MQTT return and deferred BLE recovery");
 puts("PASS weather resource use: TLS freed before JSON parse, capped automatic backoff, manual refresh, success/reconnect reset and clock wrap");
 puts("PASS weather memory recovery: field heap values, BLE/OTA exclusion, request failures, pause rollback, lock contention and MQTT recreation retry");
 puts("PASS weather: direct HTTPS without MQTT, automatic IP, city search and persistence, seven-day parsing, stale fallback and serialized network recovery");
}
'''.replace('SOURCE',source).replace('INDICATOR_TICK',indicator_tick)
folder=root/'tmp/weather'
folder.mkdir(parents=True,exist_ok=True)
path=folder/'weather_test.c'
path.write_text(test,encoding='utf-8')
exe=path.with_suffix('.exe')
subprocess.run([sys.argv[1],'-std=c11','-Os','-Wall','-Wextra','-Werror','-I'+str(root/'full'),'-I'+str(root/'main'),'-I'+str(root.parent/'esp32_direct/main/direct'),
                '-I'+str(sdk),str(path),str(sdk/'cJSON.c'),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)

assert 'esp_log_level_set("weather", ESP_LOG_INFO);' in (root/'full/app_main.c').read_text(encoding='utf-8')
