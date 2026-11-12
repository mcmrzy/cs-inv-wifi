#include "panel_weather.h"
#include "mqtt_handler.h"
#include "wifi_sta.h"
#include "ble_prov.h"
#include "ota_service.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef CONFIG_PANEL_IP_LOCATION_URL
#define CONFIG_PANEL_IP_LOCATION_URL "https://ipwho.is/?fields=success,city,latitude,longitude"
#endif
#ifndef CONFIG_PANEL_FORECAST_URL
#define CONFIG_PANEL_FORECAST_URL "https://api.open-meteo.com/v1/forecast"
#endif
#ifndef CONFIG_PANEL_CITY_SEARCH_URL
#define CONFIG_PANEL_CITY_SEARCH_URL "https://geocoding-api.open-meteo.com/v1/search"
#endif

enum { RESPONSE_MAX=4096, REFRESH_MS=1800000, RETRY_MS=60000, STALE_MS=7200000,
       SETTLE_MS=250, ADMIT_MS=1500, MIN_HEAP=36*1024, MIN_BLOCK=12*1024 };
typedef struct {
    char city[PANEL_WEATHER_CITY_BYTES], country[3];
    int32_t latitude_e6,longitude_e6;
} weather_place_t;
typedef struct {
    char *body;
    size_t used;
    bool failed;
} http_body_t;

static StaticSemaphore_t mutex_store;
static SemaphoreHandle_t mutex;
static panel_weather_snapshot_t view;
static weather_place_t saved_city, ip_city, candidates[PANEL_WEATHER_CITIES];
static nvs_handle_t prefs;
static bool prefs_ready;
static bool wifi_online, wanted=true, search_pending;
static uint32_t retry_ms, last_fetch_ms, wifi_epoch;
static char search_name[33];
static atomic_bool network_owned, worker_done;
static atomic_uint release_ms;
static bool mqtt_paused, awaiting_heap;
extern bool full_mqtt_client_resident(void);

static uint32_t clock_ms(void){return (uint32_t)(esp_timer_get_time()/1000);}
static bool lock(void){return mutex && xSemaphoreTake(mutex,pdMS_TO_TICKS(50))==pdTRUE;}
static void unlock(void){xSemaphoreGive(mutex);}
static bool due(uint32_t now,uint32_t deadline){return (int32_t)(now-deadline)>=0;}
static int32_t round_value(double value){return (int32_t)(value+(value<0?-0.5:0.5));}

static bool place_valid(const weather_place_t *place)
{
    return place->city[0] && memchr(place->city,0,sizeof(place->city)) &&
        place->latitude_e6>=-90000000 && place->latitude_e6<=90000000 &&
        place->longitude_e6>=-180000000 && place->longitude_e6<=180000000;
}

static bool read_place(cJSON *obj,const char *name,weather_place_t *out)
{
    const cJSON *city=cJSON_GetObjectItemCaseSensitive(obj,name);
    const cJSON *lat=cJSON_GetObjectItemCaseSensitive(obj,"latitude");
    const cJSON *lon=cJSON_GetObjectItemCaseSensitive(obj,"longitude");
    if(!cJSON_IsString(city) || !city->valuestring[0] || strlen(city->valuestring)>=sizeof(out->city) ||
       !cJSON_IsNumber(lat) || !cJSON_IsNumber(lon) || !isfinite(lat->valuedouble) || !isfinite(lon->valuedouble) ||
       fabs(lat->valuedouble)>90 || fabs(lon->valuedouble)>180)return false;
    for(const unsigned char *p=(const unsigned char *)city->valuestring;*p;p++)if(*p<32 || *p==127)return false;
    memset(out,0,sizeof(*out));
    strcpy(out->city,city->valuestring);
    out->latitude_e6=round_value(lat->valuedouble*1000000);
    out->longitude_e6=round_value(lon->valuedouble*1000000);
    const cJSON *country=cJSON_GetObjectItemCaseSensitive(obj,"country_code");
    if(cJSON_IsString(country) && strlen(country->valuestring)==2)
        memcpy(out->country,country->valuestring,2);
    return true;
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    if(event->event_id!=HTTP_EVENT_ON_DATA)return ESP_OK;
    http_body_t *r=event->user_data;
    if(event->data_len<0 || (size_t)event->data_len>RESPONSE_MAX-r->used){
        r->failed=true;
        return ESP_FAIL;
    }
    if(!r->body)r->body=malloc(RESPONSE_MAX+1);
    if(!r->body){r->failed=true;return ESP_FAIL;}
    memcpy(r->body+r->used,event->data,(size_t)event->data_len);
    r->used+=(size_t)event->data_len;
    r->body[r->used]=0;
    return ESP_OK;
}

static cJSON *fetch_json(const char *url,bool *timed_out)
{
    if(strncmp(url,"https://",8))return NULL;
    http_body_t r={0};
    esp_http_client_config_t config={
        .url=url,.timeout_ms=4000,.crt_bundle_attach=esp_crt_bundle_attach,
        .event_handler=http_event,.user_data=&r,.buffer_size=1024,
        .buffer_size_tx=512,.disable_auto_redirect=true,
    };
    cJSON *json=NULL;
    esp_http_client_handle_t client=esp_http_client_init(&config);
    if(client){
        esp_err_t error=esp_http_client_perform(client);
        if(error==ESP_ERR_HTTP_EAGAIN || error==ESP_ERR_TIMEOUT)*timed_out=true;
        int status=esp_http_client_get_status_code(client);
        if(error==ESP_OK && !r.failed && r.used && status==200)
            json=cJSON_ParseWithLengthOpts(r.body,r.used+1,NULL,true);
        if(!json)ESP_LOGW("weather","HTTP err=%x status=%d bytes=%u",error,status,(unsigned)r.used);
        esp_http_client_cleanup(client);
    }
    free(r.body);
    return json;
}

static bool json_number(const cJSON *item,double min,double max,double *out)
{
    if(!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
       item->valuedouble<min || item->valuedouble>max)return false;
    *out=item->valuedouble;
    return true;
}

static bool local_date(double seconds,double offset,char date[11])
{
    time_t stamp=(time_t)(seconds+offset);
    struct tm local;
    if(!gmtime_r(&stamp,&local))return false;
    unsigned year=(unsigned)(local.tm_year+1900),month=(unsigned)(local.tm_mon+1),day=(unsigned)local.tm_mday;
    if(year>2100 || month>12 || day>31)return false;
    return snprintf(date,11,"%04u-%02u-%02u",year,month,day)==10;
}

static bool parse_forecast(cJSON *root,panel_weather_snapshot_t *out)
{
    const cJSON *current=cJSON_GetObjectItemCaseSensitive(root,"current");
    const cJSON *daily=cJSON_GetObjectItemCaseSensitive(root,"daily");
    double temp,code,stamp,offset;
    if(!json_number(cJSON_GetObjectItemCaseSensitive(root,"utc_offset_seconds"),-43200,50400,&offset) ||
       !json_number(cJSON_GetObjectItemCaseSensitive(current,"temperature_2m"),-100,100,&temp) ||
       !json_number(cJSON_GetObjectItemCaseSensitive(current,"weather_code"),0,99,&code) || floor(code)!=code ||
       !json_number(cJSON_GetObjectItemCaseSensitive(current,"time"),1577836800,4102444799.0,&stamp))return false;
    time_t now=time(NULL);
    if(now>=1577836800 && ((double)now-stamp>7200 || stamp-(double)now>900))return false;
    const cJSON *dates=cJSON_GetObjectItemCaseSensitive(daily,"time");
    const cJSON *min=cJSON_GetObjectItemCaseSensitive(daily,"temperature_2m_min");
    const cJSON *max=cJSON_GetObjectItemCaseSensitive(daily,"temperature_2m_max");
    const cJSON *codes=cJSON_GetObjectItemCaseSensitive(daily,"weather_code");
    if(!cJSON_IsArray(dates) || !cJSON_IsArray(min) || !cJSON_IsArray(max) || !cJSON_IsArray(codes) ||
       cJSON_GetArraySize(dates)!=PANEL_WEATHER_DAYS || cJSON_GetArraySize(min)!=PANEL_WEATHER_DAYS ||
       cJSON_GetArraySize(max)!=PANEL_WEATHER_DAYS || cJSON_GetArraySize(codes)!=PANEL_WEATHER_DAYS)return false;
    char today[11];
    if(!local_date(stamp,offset,today))return false;
    double previous=0;
    for(unsigned i=0;i<PANEL_WEATHER_DAYS;i++){
        double date,low,high,day_code;
        panel_weather_day_t *day=&out->daily[i];
        if(!json_number(cJSON_GetArrayItem(dates,i),1577836800,4102444799.0,&date) ||
           (i && (date-previous<82800 || date-previous>90000)) || !local_date(date,offset,day->date) ||
           (!i && strcmp(day->date,today)) || (i && strcmp(day->date,out->daily[i-1].date)<=0) ||
           !json_number(cJSON_GetArrayItem(min,i),-100,100,&low) ||
           !json_number(cJSON_GetArrayItem(max,i),low,100,&high) ||
           !json_number(cJSON_GetArrayItem(codes,i),0,99,&day_code) || floor(day_code)!=day_code)return false;
        day->low_10=(int16_t)round_value(low*10);day->high_10=(int16_t)round_value(high*10);day->code=(uint16_t)day_code;
        previous=date;
    }
    out->temperature_10=(int16_t)round_value(temp*10);
    out->code=(uint16_t)code;
    out->updated_at=(uint32_t)stamp;
    out->days=PANEL_WEATHER_DAYS;
    return true;
}

static int search_cities(const char *name,weather_place_t found[PANEL_WEATHER_CITIES],bool *timed_out)
{
    char url[384];
    int start=snprintf(url,sizeof(url),"%s?count=3&language=en&name=",CONFIG_PANEL_CITY_SEARCH_URL);
    if(start<=0 || (size_t)start+3*strlen(name)>=sizeof(url))return -1;
    char *at=url+start;
    for(const unsigned char *c=(const unsigned char *)name;*c;c++){
        if((*c>='A' && *c<='Z') || (*c>='a' && *c<='z') || (*c>='0' && *c<='9'))*at++=(char)*c;
        else{snprintf(at,4,"%%%02X",(unsigned)*c);at+=3;}
    }
    *at=0;
    cJSON *root=fetch_json(url,timed_out);
    if(!root)return -1;
    const cJSON *results=cJSON_GetObjectItemCaseSensitive(root,"results");
    unsigned count=0;
    for(int i=0;i<cJSON_GetArraySize(results) && count<PANEL_WEATHER_CITIES;i++)
        if(read_place(cJSON_GetArrayItem(results,i),"name",&found[count]))count++;
    cJSON_Delete(root);
    return count;
}

static void weather_task(void *arg)
{
    uint32_t epoch=(uint32_t)(uintptr_t)arg;
    char name[sizeof(search_name)];
    weather_place_t place={0};
    bool configured=false, search=false, timed_out=false;
    if(lock()){
        search=search_pending;
        if(search){strcpy(name,search_name);search_pending=false;}
        configured=place_valid(&saved_city);
        place=configured?saved_city:ip_city;
        unlock();
    }
    if(search){
        weather_place_t found[PANEL_WEATHER_CITIES]={0};
        int count=search_cities(name,found,&timed_out);
        if(lock()){
            view.city_count=epoch==wifi_epoch && count>0?(uint8_t)count:0;
            memcpy(candidates,found,sizeof(candidates));
            view.search_state=view.city_count?PANEL_CITY_RESULTS:
                count<0 || epoch!=wifi_epoch?PANEL_CITY_ERROR:PANEL_CITY_EMPTY;
            unlock();
        }
    }else{
        if(!place_valid(&place)){
            cJSON *location=fetch_json(CONFIG_PANEL_IP_LOCATION_URL,&timed_out);
            if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(location,"success")))read_place(location,"city",&place);
            cJSON_Delete(location);
        }
        panel_weather_snapshot_t forecast={0};
        bool ok=false;
        if(place_valid(&place)){
            char url[384];
            int n=snprintf(url,sizeof(url),"%s?latitude=%.6f&longitude=%.6f&timezone=auto&timeformat=unixtime"
                "&current=temperature_2m,weather_code&daily=temperature_2m_min,temperature_2m_max,weather_code&forecast_days=7",
                CONFIG_PANEL_FORECAST_URL,place.latitude_e6/1000000.0,place.longitude_e6/1000000.0);
            cJSON *json=n>0 && n<(int)sizeof(url)?fetch_json(url,&timed_out):NULL;
            ok=parse_forecast(json,&forecast);
            cJSON_Delete(json);
        }
        if(lock()){
            if(epoch==wifi_epoch){
                if(place_valid(&place) && !configured)ip_city=place;
                if(ok){
                    strcpy(view.city,place.city);
                    view.configured_city=configured;view.location_valid=true;view.valid=true;
                    view.temperature_10=forecast.temperature_10;view.code=forecast.code;
                    view.updated_at=forecast.updated_at;view.days=forecast.days;
                    memcpy(view.daily,forecast.daily,sizeof(view.daily));
                    last_fetch_ms=clock_ms();view.state=PANEL_WEATHER_READY;
                }else view.state=timed_out?PANEL_WEATHER_TIMEOUT:
                    place_valid(&place)?PANEL_WEATHER_ERROR:PANEL_WEATHER_LOCATION_FAILED;
                retry_ms=clock_ms()+(ok?REFRESH_MS:RETRY_MS);
            }
            unlock();
        }
    }
    atomic_store(&release_ms,clock_ms());
    atomic_store(&worker_done,true);
    vTaskDelete(NULL);
}

void panel_weather_init(void)
{
    if(mutex)return;
    mutex=xSemaphoreCreateMutexStatic(&mutex_store);
    prefs_ready=nvs_open("panel_weather",NVS_READWRITE,&prefs)==ESP_OK;
    size_t size=sizeof(saved_city);
    if(!prefs_ready || nvs_get_blob(prefs,"city",&saved_city,&size)!=ESP_OK ||
       size!=sizeof(saved_city) || !place_valid(&saved_city))memset(&saved_city,0,sizeof(saved_city));
}

bool panel_weather_network_busy(void){return atomic_load(&network_owned);}

static void admit_weather(uint32_t now,bool reserved)
{
    uint32_t elapsed=now-atomic_load(&release_ms);
    if(elapsed<SETTLE_MS || !lock())return;
    unsigned caps=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT;
    unsigned free=heap_caps_get_free_size(caps),largest=heap_caps_get_largest_free_block(caps);
    bool usable=!reserved && wifi_sta_is_connected() && !ble_prov_is_connected();
    if(usable && (free<MIN_HEAP || largest<MIN_BLOCK) && elapsed<ADMIT_MS){unlock();return;}
    awaiting_heap=false;
    if(usable && free>=MIN_HEAP && largest>=MIN_BLOCK &&
       xTaskCreate(weather_task,"panel_weather",8192,(void *)(uintptr_t)wifi_epoch,4,NULL)==pdPASS){
        ESP_LOGI("weather","start heap=%u block=%u",free,largest);
        unlock();
        return;
    }
    ESP_LOGW("weather","defer heap=%u block=%u",free,largest);
    if(search_pending){search_pending=false;view.search_state=PANEL_CITY_ERROR;}
    else{view.state=PANEL_WEATHER_ERROR;retry_ms=now+RETRY_MS;}
    atomic_store(&release_ms,now);
    atomic_store(&worker_done,true);
    unlock();
}

void panel_weather_process(bool reserved)
{
    uint32_t now=clock_ms();
    if(panel_weather_network_busy()){
        if(awaiting_heap){admit_weather(now,reserved);return;}
        if(atomic_load(&worker_done) && now-atomic_load(&release_ms)>=SETTLE_MS){
            if(mqtt_paused)mqtt_resume_after_local();
            mqtt_paused=false;
            ota_service_admin_end();
            atomic_store(&network_owned,false);
        }else if(!wifi_sta_is_connected() && lock()){
            if(wifi_online){wifi_online=false;wifi_epoch++;ip_city.city[0]=0;view.state=PANEL_WEATHER_OFFLINE;}
            unlock();
        }
        return;
    }
    if(!lock())return;
    bool wifi=wifi_sta_is_connected();
    if(wifi!=wifi_online){
        wifi_online=wifi;wifi_epoch++;wanted=true;retry_ms=now;ip_city.city[0]=0;
        view.state=wifi?PANEL_WEATHER_IDLE:PANEL_WEATHER_OFFLINE;
        if(!wifi && search_pending){search_pending=false;view.search_state=PANEL_CITY_ERROR;}
    }
    bool pending=wifi && (search_pending || (due(now,retry_ms) && (wanted || view.state!=PANEL_WEATHER_IDLE)));
    unlock();
    if(!pending || reserved || mqtt_is_connecting() || ble_prov_is_connected() || !ota_service_admin_begin())return;
    if(mqtt_is_connected()){
        unsigned caps=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT;
        if(heap_caps_get_free_size(caps)<MIN_HEAP ||
           heap_caps_get_largest_free_block(caps)<MIN_BLOCK){
            if(lock()){
                if(search_pending){search_pending=false;view.search_state=PANEL_CITY_ERROR;}
                else{wanted=false;view.state=PANEL_WEATHER_ERROR;retry_ms=now+RETRY_MS;}
                unlock();
            }
            ota_service_admin_end();
            return;
        }
    }
    bool release_client=full_mqtt_client_resident() && !mqtt_is_connected();
    if(release_client && !mqtt_pause_for_local()){ota_service_admin_end();return;}
    mqtt_paused=release_client;
    if(!lock()){
        if(mqtt_paused)mqtt_resume_after_local();
        mqtt_paused=false;ota_service_admin_end();return;
    }
    bool searching=search_pending;
    if(!searching){
        wanted=false;
        view.state=place_valid(&saved_city)||place_valid(&ip_city)?PANEL_WEATHER_LOADING:PANEL_WEATHER_LOCATING;
    }
    atomic_store(&worker_done,false);
    atomic_store(&network_owned,true);
    atomic_store(&release_ms,now);
    awaiting_heap=true;
    unlock();
}

void panel_weather_refresh(void)
{
    if(lock()){
        wanted=true;retry_ms=clock_ms();
        unlock();
    }
}

bool panel_weather_search(const char *city)
{
    if(!city || strlen(city)<2 || strlen(city)>=sizeof(search_name) || !wifi_sta_is_connected() || !lock())return false;
    if(view.search_state==PANEL_CITY_SEARCHING){unlock();return false;}
    strcpy(search_name,city);search_pending=true;view.city_count=0;view.search_state=PANEL_CITY_SEARCHING;
    unlock();
    return true;
}

bool panel_weather_set_city(int index)
{
    if(!lock())return false;
    if(!prefs_ready || panel_weather_network_busy() || index < -1 || index>=view.city_count){unlock();return false;}
    weather_place_t next={0};
    if(index>=0)next=candidates[index];
    bool saved=nvs_set_blob(prefs,"city",&next,sizeof(next))==ESP_OK && nvs_commit(prefs)==ESP_OK;
    if(saved){
        saved_city=next;
        view.valid=false;view.location_valid=false;view.days=0;view.city[0]=0;
        wanted=true;retry_ms=clock_ms();view.state=PANEL_WEATHER_IDLE;view.search_state=PANEL_CITY_IDLE;view.city_count=0;
    }
    unlock();
    return saved;
}

bool panel_weather_city(unsigned index,char *out,size_t capacity)
{
    if(!out || !capacity || !lock())return false;
    bool valid=index<view.city_count;
    if(valid)snprintf(out,capacity,"%.38s%s%s",candidates[index].city,candidates[index].country[0]?", ":"",candidates[index].country);
    unlock();
    return valid;
}

bool panel_weather_snapshot(panel_weather_snapshot_t *out)
{
    if(!out || !lock())return false;
    *out=view;
    out->age_ms=view.valid?clock_ms()-last_fetch_ms:UINT32_MAX;
    out->stale=view.valid && (!wifi_online || out->age_ms>=STALE_MS ||
        view.state==PANEL_WEATHER_ERROR || view.state==PANEL_WEATHER_TIMEOUT || view.state==PANEL_WEATHER_LOCATION_FAILED);
    unlock();
    return true;
}
