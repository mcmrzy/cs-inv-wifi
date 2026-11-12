#include "panel_keys.h"
#include "direct_key_codec.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

enum { PIN_KEY_DAT=2, PIN_KEY_CLK=3 };
static QueueHandle_t s_keys;
static portMUX_TYPE s_clock_lock=portMUX_INITIALIZER_UNLOCKED;

static int sample(void)
{
    if(gpio_get_level(PIN_KEY_DAT))return DIRECT_KEY_NO_FRAME;
    uint16_t raw=0;
    taskENTER_CRITICAL(&s_clock_lock);
    if(gpio_get_level(PIN_KEY_DAT)) {
        taskEXIT_CRITICAL(&s_clock_lock);return DIRECT_KEY_NO_FRAME;
    }
    for(unsigned bit=0;bit<16;bit++) {
        esp_rom_delay_us(25);gpio_set_level(PIN_KEY_CLK,0);
        esp_rom_delay_us(25);
        raw|=(uint16_t)(gpio_get_level(PIN_KEY_DAT)<<bit);
        gpio_set_level(PIN_KEY_CLK,1);
    }
    taskEXIT_CRITICAL(&s_clock_lock);
    return direct_key_decode(raw);
}

static void key_task(void *arg)
{
    (void)arg;
    direct_key_state_t state={0};uint32_t next=0;
    for(;;) {
        uint32_t now=(uint32_t)(esp_timer_get_time()/1000);
        if((int32_t)(now-next)>=0) {
            int frame=sample();
            next=now+direct_key_scan_interval(frame);
            uint8_t key=direct_key_update(&state,frame,now);
            if(key && key!=DIRECT_KEY_ENTER_BACK &&
               (state.press_edge || uxQueueSpacesAvailable(s_keys)>4U)) {
                if(xQueueSend(s_keys,&key,state.press_edge ? pdMS_TO_TICKS(50) : 0)!=pdTRUE)
                    ESP_LOGW("panel_keys","Key queue full: %u",key);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t panel_keys_init(void)
{
    gpio_set_level(PIN_KEY_CLK,1);
    gpio_config_t clk={.pin_bit_mask=1ULL<<PIN_KEY_CLK,.mode=GPIO_MODE_OUTPUT};
    gpio_config_t dat={.pin_bit_mask=1ULL<<PIN_KEY_DAT,.mode=GPIO_MODE_INPUT,.pull_up_en=GPIO_PULLUP_ENABLE};
    esp_err_t err=gpio_config(&clk);if(err!=ESP_OK)return err;
    err=gpio_config(&dat);if(err!=ESP_OK)return err;
    s_keys=xQueueCreate(32,sizeof(uint8_t));if(!s_keys)return ESP_ERR_NO_MEM;
    if(xTaskCreate(key_task,"panel_keys",3072,NULL,6,NULL)!=pdPASS)return ESP_ERR_NO_MEM;
    ESP_LOGI("panel_keys","BS818A DAT=GPIO2 CLK=GPIO3, edge confirm/back; direction repeats");
    return ESP_OK;
}

bool panel_keys_next(uint8_t *key)
{ return key && s_keys && xQueueReceive(s_keys,key,0)==pdTRUE; }
