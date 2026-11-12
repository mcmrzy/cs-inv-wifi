#pragma once
#include <stdint.h>
#include <stddef.h>
typedef int esp_err_t;
typedef int nvs_handle_t;
typedef struct {uint8_t bytes[128];unsigned head,count;size_t item;} StaticQueue_t;
typedef StaticQueue_t *QueueHandle_t;
enum {ESP_OK=0,ESP_ERR_NVS_NOT_FOUND=1,ESP_FAIL=2,NVS_READWRITE=1,pdTRUE=1};
extern char g_device_sn[33];
void mock_alarm_log(const char *,const char *,...);
#define ESP_LOGI mock_alarm_log
#define ESP_LOGW mock_alarm_log
#define ESP_LOGE mock_alarm_log
int64_t esp_timer_get_time(void);
const char *esp_err_to_name(int);
QueueHandle_t xQueueCreateStatic(unsigned,size_t,uint8_t *,StaticQueue_t *);
int xQueueSend(QueueHandle_t,const void *,unsigned);
int xQueueReceive(QueueHandle_t,void *,unsigned);
esp_err_t nvs_open(const char *,int,nvs_handle_t *);
esp_err_t nvs_get_blob(nvs_handle_t,const char *,void *,size_t *);
esp_err_t nvs_set_blob(nvs_handle_t,const char *,const void *,size_t);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
bool mqtt_is_connected(void);
bool ota_service_is_busy(void);
bool panel_bus_alarm_sample(panel_alarm_sample_t *);
