#include "direct_buzzer.h"
#include "sdkconfig.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* Passive 4kHz buzzer on its own timer/channel. Backlight owns timer0/channel0. */
#ifndef CONFIG_PANEL_BUZZ_FREQUENCY_HZ
#define CONFIG_PANEL_BUZZ_FREQUENCY_HZ 4000
#endif
#define BUZZ_DURATION_US 70000
static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;
static portMUX_TYPE s_init_mux=portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t s_timer;
static bool s_enabled=true,s_pwm_ready;
static uint8_t s_volume=100;
static int64_t s_off_at;

static bool take(void)
{
    taskENTER_CRITICAL(&s_init_mux);
    if(!s_lock)s_lock=xSemaphoreCreateMutexStatic(&s_lock_storage);
    taskEXIT_CRITICAL(&s_init_mux);
    return s_lock && xSemaphoreTake(s_lock,portMAX_DELAY)==pdTRUE;
}
static void output(bool on)
{
#if CONFIG_DIRECT_BUZZ_PIN >= 0
    if(!s_pwm_ready)return;
    uint32_t duty=on?((uint32_t)s_volume*512U+50U)/100U:0;
    esp_err_t err=ledc_set_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_1,duty);
    if(err==ESP_OK)err=ledc_update_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_1);
    if(err!=ESP_OK){
        ledc_stop(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_1,!CONFIG_DIRECT_BUZZ_ACTIVE_LEVEL);
        s_pwm_ready=false;s_off_at=0;
    }
#else
    (void)on;
#endif
}
static void timer_tick(void *arg)
{
    (void)arg;
    if(!take())return;
    if(s_off_at && esp_timer_get_time()>=s_off_at){s_off_at=0;output(false);}
    xSemaphoreGive(s_lock);
}
esp_err_t direct_buzzer_init(void)
{
    if(!take())return ESP_ERR_NO_MEM;
    if(s_timer){xSemaphoreGive(s_lock);return ESP_OK;}
    uint16_t enabled=1;nvs_handle_t h;
    if(nvs_open("direct_ui",NVS_READONLY,&h)==ESP_OK){
        (void)nvs_get_u16(h,"buzzer",&enabled);nvs_close(h);
    }
    s_enabled=enabled!=0;
    esp_err_t err=ESP_OK;
#if CONFIG_DIRECT_BUZZ_PIN >= 0
    const ledc_timer_config_t tone={.speed_mode=LEDC_LOW_SPEED_MODE,
        .duty_resolution=LEDC_TIMER_10_BIT,.timer_num=LEDC_TIMER_1,
        .freq_hz=CONFIG_PANEL_BUZZ_FREQUENCY_HZ,.clk_cfg=LEDC_AUTO_CLK};
    err=ledc_timer_config(&tone);
    if(err==ESP_OK){
        const ledc_channel_config_t channel={.gpio_num=CONFIG_DIRECT_BUZZ_PIN,
            .speed_mode=LEDC_LOW_SPEED_MODE,.channel=LEDC_CHANNEL_1,
            .timer_sel=LEDC_TIMER_1,.intr_type=LEDC_INTR_DISABLE,.duty=0,.hpoint=0,
            .flags.output_invert=!CONFIG_DIRECT_BUZZ_ACTIVE_LEVEL};
        err=ledc_channel_config(&channel);
    }
    if(err==ESP_OK)s_pwm_ready=true;
#endif
    if(err==ESP_OK){
        const esp_timer_create_args_t args={.callback=timer_tick,.name="panel_beep"};
        esp_timer_handle_t timer;
        err=esp_timer_create(&args,&timer);
        if(err==ESP_OK){
            err=esp_timer_start_periodic(timer,5000);
            if(err==ESP_OK)s_timer=timer;else esp_timer_delete(timer);
        }
    }
    if(err!=ESP_OK)output(false);
    xSemaphoreGive(s_lock);return err;
}
void direct_buzzer_set_enabled(bool enabled)
{
    if(!take())return;
    s_enabled=enabled;
    if(!enabled){s_off_at=0;output(false);}
    xSemaphoreGive(s_lock);
}
void direct_buzzer_set_duration_ms(uint16_t duration_ms)
{
    /* Retained API; panel key/alarm tones deliberately stay at 70ms. */
    (void)duration_ms;
}
void direct_buzzer_set_volume(uint8_t percent)
{
    if(!take())return;
    s_volume=percent>100?100:percent;
    if(!s_volume){s_off_at=0;output(false);}
    else if(s_off_at)output(true);
    xSemaphoreGive(s_lock);
}
void direct_buzzer_pulse(void)
{
    if(!take())return;
    if(s_enabled && s_timer && s_volume){
        s_off_at=esp_timer_get_time()+BUZZ_DURATION_US;output(true);
    }
    xSemaphoreGive(s_lock);
}
