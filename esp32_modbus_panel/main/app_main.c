#include "panel_bus.h"
#include "panel_keys.h"
#include "panel_lcd.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static panel_snapshot_t s_snapshot;
static panel_ui_t s_ui;
static panel_view_t s_view,s_previous;

void app_main(void)
{
    panel_lcd_prepare();
    esp_err_t err=nvs_flash_init();
    /* Do not erase existing identity/configuration on an incompatible NVS. */
    if(err!=ESP_OK)ESP_LOGE("panel","NVS unavailable: %s; local config cannot be saved",esp_err_to_name(err));
    panel_ui_init(&s_ui);
    err=panel_keys_init();
    if(err!=ESP_OK)ESP_LOGE("panel","Keys unavailable: %s",esp_err_to_name(err));
    bool lcd=panel_lcd_init()==ESP_OK;
    err=panel_bus_start();
    if(err!=ESP_OK)ESP_LOGE("panel","Bus unavailable: %s",esp_err_to_name(err));
    bool drawn=false;uint32_t last_retry=0;
    for(;;) {
        (void)panel_bus_snapshot(&s_snapshot);
        panel_ui_sync(&s_ui,&s_snapshot);
        uint8_t key;
        while(panel_keys_next(&key)) {
            panel_command_t command={0};
            if(panel_ui_key(&s_ui,key,&s_snapshot,&command)){
                uint32_t sequence;
                if(panel_bus_submit_sequence(&command,&sequence))panel_ui_submitted(&s_ui,sequence,command.type);
                else panel_ui_submit_failed(&s_ui,command.type);
            }
        }
        panel_ui_view(&s_ui,&s_snapshot,&s_view);
        if(lcd && (!drawn || memcmp(&s_view,&s_previous,sizeof(s_view)))) {
            err=panel_lcd_render(&s_view);
            if(err==ESP_OK){s_previous=s_view;drawn=true;}
            else {lcd=false;drawn=false;ESP_LOGE("panel","LCD transfer failed: %s",esp_err_to_name(err));}
        }
        uint32_t now=(uint32_t)(esp_timer_get_time()/1000);
        if(!lcd && now-last_retry>=2000U) {
            last_retry=now;lcd=panel_lcd_init()==ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
