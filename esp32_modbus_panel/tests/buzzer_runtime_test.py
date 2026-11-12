"""Exercise the panel's actual passive-buzzer driver and independent PWM owner."""
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
body = re.sub(r'^#include[^\n]*\n', '', (root/'full/panel_buzzer.c').read_text(encoding='utf-8'), flags=re.M)
test = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
typedef int esp_err_t,StaticSemaphore_t,portMUX_TYPE,nvs_handle_t;
typedef void *SemaphoreHandle_t;
typedef void *esp_timer_handle_t;
typedef struct {void (*callback)(void *);const char *name;} esp_timer_create_args_t;
typedef struct {int speed_mode,duty_resolution,timer_num,freq_hz,clk_cfg;} ledc_timer_config_t;
typedef struct {int gpio_num,speed_mode,channel,timer_sel,intr_type,duty,hpoint;
 struct {bool output_invert;} flags;} ledc_channel_config_t;
enum {ESP_OK=0,ESP_ERR_NO_MEM=1,NVS_READONLY=2,LEDC_LOW_SPEED_MODE=0,
 LEDC_CHANNEL_1=1,LEDC_TIMER_1=1,LEDC_TIMER_10_BIT=10,LEDC_AUTO_CLK=3,LEDC_INTR_DISABLE=0};
#define CONFIG_DIRECT_BUZZ_PIN 10
#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(p) ((void)(p))
#define taskEXIT_CRITICAL(p) ((void)(p))
#define portMAX_DELAY 0xffffffffU
#define pdTRUE 1
static int held,fail_stage,deleted;
static unsigned duty,stops,timers;
static int64_t clock_us;
static uint16_t legacy_enabled=1;
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *p){return p;}
static int xSemaphoreTake(SemaphoreHandle_t p,unsigned delay){(void)p;(void)delay;assert(!held);held=1;return 1;}
static void xSemaphoreGive(SemaphoreHandle_t p){(void)p;assert(held);held=0;}
static int64_t esp_timer_get_time(void){return clock_us;}
static int nvs_open(const char *name,int mode,nvs_handle_t *out){(void)name;(void)mode;*out=1;return 0;}
static int nvs_get_u16(nvs_handle_t h,const char *key,uint16_t *out){(void)h;(void)key;*out=legacy_enabled;return 0;}
static void nvs_close(nvs_handle_t h){(void)h;}
static int ledc_timer_config(const ledc_timer_config_t *p){
 assert(p->timer_num==LEDC_TIMER_1&&p->freq_hz==4000&&p->duty_resolution==10);
 timers++;return fail_stage==1?1:0;
}
static int ledc_channel_config(const ledc_channel_config_t *p){
 assert(p->gpio_num==10&&p->channel==LEDC_CHANNEL_1&&p->timer_sel==LEDC_TIMER_1);
 assert(p->flags.output_invert==!CONFIG_DIRECT_BUZZ_ACTIVE_LEVEL&&p->duty==0);
 duty=0;return fail_stage==2?1:0;
}
static int ledc_set_duty(int mode,int channel,uint32_t value){
 assert(held&&mode==LEDC_LOW_SPEED_MODE&&channel==LEDC_CHANNEL_1&&value<=512);
 if(fail_stage==3)return 1;duty=value;return 0;
}
static int ledc_update_duty(int mode,int channel){(void)mode;(void)channel;return fail_stage==4?1:0;}
static int ledc_stop(int mode,int channel,int idle){
 assert(mode==LEDC_LOW_SPEED_MODE&&channel==LEDC_CHANNEL_1&&idle==!CONFIG_DIRECT_BUZZ_ACTIVE_LEVEL);
 stops++;duty=0;return 0;
}
static int esp_timer_create(const esp_timer_create_args_t *p,esp_timer_handle_t *out){
 assert(p->callback);if(fail_stage==5)return 1;*out=(void *)1;return 0;
}
static int esp_timer_start_periodic(esp_timer_handle_t h,unsigned period){(void)h;assert(period==5000);return fail_stage==6?1:0;}
static int esp_timer_delete(esp_timer_handle_t h){(void)h;deleted++;return 0;}
SOURCE
static void reset(void){s_timer=NULL;s_pwm_ready=false;s_enabled=true;s_volume=100;s_off_at=0;
 fail_stage=0;held=0;duty=stops=timers=0;clock_us=0;legacy_enabled=1;}
int main(void){
 reset();assert(direct_buzzer_init()==ESP_OK&&timers==1);
 assert(direct_buzzer_init()==ESP_OK&&timers==1);
 direct_buzzer_pulse();assert(duty==512&&s_off_at==70000);
 clock_us=69999;timer_tick(NULL);assert(duty==512);
 clock_us=70000;timer_tick(NULL);assert(!duty&&!s_off_at);
 direct_buzzer_set_duration_ms(120);direct_buzzer_pulse();assert(s_off_at==140000);
 clock_us=80000;direct_buzzer_pulse();assert(s_off_at==150000);
 clock_us=140000;timer_tick(NULL);assert(duty==512);
 direct_buzzer_set_volume(25);assert(duty==128);
 direct_buzzer_set_volume(60);assert(duty==307);
 direct_buzzer_set_volume(255);assert(duty==512);
 direct_buzzer_set_enabled(false);assert(!duty&&!s_off_at);
 direct_buzzer_pulse();assert(!duty);
 direct_buzzer_set_enabled(true);direct_buzzer_set_volume(0);direct_buzzer_pulse();assert(!duty);
 direct_buzzer_set_volume(100);direct_buzzer_pulse();assert(duty==512);
 fail_stage=3;direct_buzzer_set_volume(25);assert(!duty&&!s_pwm_ready&&!s_off_at&&stops==1);
 for(int stage=1;stage<=6;stage++){
  reset();fail_stage=stage;
  if(stage==3||stage==4){assert(direct_buzzer_init()==ESP_OK);direct_buzzer_pulse();assert(!duty&&!s_pwm_ready);}
  else assert(direct_buzzer_init()!=ESP_OK&&!duty);
  assert(!held);
 }
 reset();legacy_enabled=0;assert(direct_buzzer_init()==ESP_OK);direct_buzzer_pulse();assert(!duty);
 puts("PASS passive buzzer: independent 4kHz PWM, 70ms fixed pulse, overlapping deadlines, volume/mute, polarity and driver failures");
}
'''.replace('SOURCE', body)
source = root/'tmp/buzzer_runtime_test.c'
source.write_text(test, encoding='utf-8')
exe = source.with_suffix('.exe')
for polarity in (0,1):
    subprocess.run([sys.argv[1], '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-misleading-indentation',
                    f'-DCONFIG_DIRECT_BUZZ_ACTIVE_LEVEL={polarity}', str(source), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
