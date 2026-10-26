#include "direct_energy.h"
#include "direct_energy_codec.h"
#include "direct_link.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>
#include <sys/time.h>
static const char *TAG="direct_energy";
static SemaphoreHandle_t s_lock;
static direct_energy_accumulator_t s_energy;
static nvs_handle_t s_nvs;
static bool s_nvs_open,s_initialized,s_timezone_dirty;

static uint32_t monotonic_ms(void) { return (uint32_t)(esp_timer_get_time()/1000); }
static void persist(void)
{
    if (!s_nvs_open || xSemaphoreTake(s_lock,pdMS_TO_TICKS(50))!=pdTRUE) return;
    const direct_energy_record_t record=s_energy.record;
    xSemaphoreGive(s_lock);
    esp_err_t result=nvs_set_blob(s_nvs,"counters",&record,sizeof(record));
    if (result==ESP_OK) result=nvs_commit(s_nvs);
    if (result!=ESP_OK) ESP_LOGW(TAG,"energy persistence failed: %s",esp_err_to_name(result));
}
static void energy_task(void *arg)
{
    (void)arg;
    uint32_t last_save=monotonic_ms();
    for (;;) {
        uint32_t power[DIRECT_ENERGY_CHANNELS]={0};
        RunParamDef measurement;
        uint32_t received=0;
        bool fresh=direct_dsp_get_measurement(&measurement,&received);
        uint32_t now=monotonic_ms();
        struct timeval tv;
        bool calendar=gettimeofday(&tv,NULL)==0 && tv.tv_sec>=1577836800LL;
        int64_t utc_ms=calendar ? (int64_t)tv.tv_sec*1000+tv.tv_usec/1000 : 0;
        if (fresh) {
            /* Align calendar time to the last real DSP04 sample, not task jitter. */
            if (calendar) utc_ms-=(uint32_t)(now-received);
            direct_energy_power(measurement.Ppv,measurement.ACInWatt,
                (int32_t)measurement.BatChrWatt-(int32_t)measurement.BatDisChrWatt,
                measurement.OutputWatt,power);
        }
        bool day_changed=false,timezone_changed=false;
        if (xSemaphoreTake(s_lock,pdMS_TO_TICKS(50))==pdTRUE) {
            day_changed=direct_energy_accumulate(&s_energy,received,utc_ms,calendar,fresh,power);
            timezone_changed=s_timezone_dirty; s_timezone_dirty=false;
            xSemaphoreGive(s_lock);
        }
        if (day_changed || timezone_changed || now-last_save>=600000U) {
            persist(); last_save=now;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
void direct_energy_init(int timezone)
{
    if (s_initialized) return;
    s_lock=xSemaphoreCreateMutex();
    if (!s_lock) { ESP_LOGE(TAG,"energy mutex allocation failed"); return; }
    direct_energy_accumulator_init(&s_energy,timezone);
    if (nvs_open(DIRECT_ENERGY_NVS_NAMESPACE,NVS_READWRITE,&s_nvs)==ESP_OK) {
        s_nvs_open=true;
        direct_energy_record_t record; size_t size=sizeof(record);
        if (nvs_get_blob(s_nvs,"counters",&record,&size)==ESP_OK && size==sizeof(record)) {
            if (!direct_energy_record_load(&s_energy,&record))
                ESP_LOGW(TAG,"invalid energy record; starting new ESP counters");
        }
        direct_energy_accumulator_timezone(&s_energy,timezone);
    } else ESP_LOGW(TAG,"energy NVS unavailable; counters remain in RAM");
    if (xTaskCreate(energy_task,"direct_energy",3072,NULL,3,NULL)!=pdPASS) {
        ESP_LOGE(TAG,"energy task allocation failed");
        if (s_nvs_open) { nvs_close(s_nvs); s_nvs_open=false; }
        vSemaphoreDelete(s_lock); s_lock=NULL; return;
    }
    s_initialized=true;
    ESP_LOGI(TAG,"ESP energy counters ready; generator channel not metered, save every 10 min");
}
void direct_energy_set_timezone(int timezone)
{
    if (!s_lock || timezone<-840 || timezone>840 ||
        xSemaphoreTake(s_lock,pdMS_TO_TICKS(50))!=pdTRUE) return;
    if (timezone!=s_energy.record.timezone_minutes) {
        direct_energy_accumulator_timezone(&s_energy,timezone); s_timezone_dirty=true;
    }
    xSemaphoreGive(s_lock);
}
bool direct_energy_snapshot(uint64_t today[DIRECT_ENERGY_CHANNELS],
                            uint64_t total[DIRECT_ENERGY_CHANNELS])
{
    if (!today || !total || !s_initialized ||
        xSemaphoreTake(s_lock,pdMS_TO_TICKS(50))!=pdTRUE) return false;
    memcpy(today,s_energy.record.today,sizeof(s_energy.record.today));
    memcpy(total,s_energy.record.total,sizeof(s_energy.record.total));
    xSemaphoreGive(s_lock); return true;
}
