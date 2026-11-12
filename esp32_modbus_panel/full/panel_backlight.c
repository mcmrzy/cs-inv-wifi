#include "panel_backlight.h"
#include "panel_display_settings.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "sdkconfig.h"
static bool ready;
static uint8_t wanted=PANEL_DISPLAY_DEFAULT_BRIGHTNESS;
static bool blank;
static uint32_t previous=UINT32_MAX;
void full_backlight_set(uint8_t brightness,bool asleep)
{
    wanted=brightness;blank=asleep;
    uint32_t duty=panel_display_duty(brightness,asleep,CONFIG_PANEL_LCD_BL_ACTIVE_LEVEL!=0);
    if(ready){
        if(duty==previous)return;
        esp_err_t err=ledc_set_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,duty);
        if(err==ESP_OK)err=ledc_update_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0);
        if(err==ESP_OK){previous=duty;return;}
        ESP_LOGW("backlight","GPIO5 PWM update failed: %s; switching to on/off",esp_err_to_name(err));
        ready=false;ledc_stop(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,!CONFIG_PANEL_LCD_BL_ACTIVE_LEVEL);
        gpio_reset_pin(CONFIG_DIRECT_LCD_BL_PIN);gpio_set_direction(CONFIG_DIRECT_LCD_BL_PIN,GPIO_MODE_OUTPUT);
    }
    if(CONFIG_DIRECT_LCD_BL_PIN>=0){
        gpio_set_level(CONFIG_DIRECT_LCD_BL_PIN,asleep?!CONFIG_PANEL_LCD_BL_ACTIVE_LEVEL:CONFIG_PANEL_LCD_BL_ACTIVE_LEVEL);
    }
}
void full_backlight_attach(void)
{
    if(CONFIG_DIRECT_LCD_BL_PIN<0)return;
    const ledc_timer_config_t timer={.speed_mode=LEDC_LOW_SPEED_MODE,.duty_resolution=LEDC_TIMER_10_BIT,
        .timer_num=LEDC_TIMER_0,.freq_hz=20000,.clk_cfg=LEDC_AUTO_CLK};
    esp_err_t err=ledc_timer_config(&timer);
    if(err==ESP_OK){
        const ledc_channel_config_t channel={.gpio_num=CONFIG_DIRECT_LCD_BL_PIN,.speed_mode=LEDC_LOW_SPEED_MODE,
            .channel=LEDC_CHANNEL_0,.timer_sel=LEDC_TIMER_0,.intr_type=LEDC_INTR_DISABLE,
            .duty=panel_display_duty(wanted,blank,CONFIG_PANEL_LCD_BL_ACTIVE_LEVEL!=0)};
        err=ledc_channel_config(&channel);
    }
    ready=err==ESP_OK;previous=UINT32_MAX;
    if(!ready){
        ESP_LOGW("backlight","GPIO5 PWM unavailable: %s; on/off fallback",esp_err_to_name(err));
        gpio_reset_pin(CONFIG_DIRECT_LCD_BL_PIN);gpio_set_direction(CONFIG_DIRECT_LCD_BL_PIN,GPIO_MODE_OUTPUT);
    }
    full_backlight_set(wanted,blank);
}
