"""Run the actual passive PWM sequencer against deterministic driver/time stubs."""
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
body = re.sub(r'^#include[^\n]*\n', '', (root/'full/panel_buzzer.c').read_text(encoding='utf-8'), flags=re.M)
header = root/'full/panel_sound.h'
api = re.sub(r'^#(?:include|pragma)[^\n]*\n', '', header.read_text(encoding='utf-8'), flags=re.M) if header.exists() else r'''
typedef enum {PANEL_SOUND_NONE=0,PANEL_SOUND_BOOT,PANEL_SOUND_KEY,PANEL_SOUND_CONFIRM,
 PANEL_SOUND_BACK,PANEL_SOUND_SUCCESS,PANEL_SOUND_ERROR,PANEL_SOUND_WARNING,
 PANEL_SOUND_FAULT,PANEL_SOUND_RECOVERY,PANEL_SOUND_COUNT} panel_sound_effect_t;
void panel_sound_play(panel_sound_effect_t);
void panel_sound_key(uint8_t);
'''
test = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <stdatomic.h>
#include <string.h>
typedef int esp_err_t,StaticSemaphore_t,portMUX_TYPE;
typedef void *SemaphoreHandle_t;
typedef void *esp_timer_handle_t;
typedef struct {void (*callback)(void *);const char *name;} esp_timer_create_args_t;
typedef struct {int speed_mode,duty_resolution,timer_num,freq_hz,clk_cfg;} ledc_timer_config_t;
typedef struct {int gpio_num,speed_mode,channel,timer_sel,intr_type,duty,hpoint;
 struct {bool output_invert;} flags;} ledc_channel_config_t;
enum {ESP_OK=0,ESP_ERR_NO_MEM=1,LEDC_LOW_SPEED_MODE=0,LEDC_CHANNEL_1=1,
 LEDC_TIMER_1=1,LEDC_TIMER_10_BIT=10,LEDC_AUTO_CLK=3,LEDC_INTR_DISABLE=0};
#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(p) ((void)(p))
#define taskEXIT_CRITICAL(p) ((void)(p))
#define pdTRUE 1
#ifndef CONFIG_PANEL_BUZZ_FREQUENCY_HZ
#define CONFIG_PANEL_BUZZ_FREQUENCY_HZ 4000
#endif
#define pdMS_TO_TICKS(x) (x)
static int held,fail_stage,deleted,contended;
static unsigned duty,frequency,stops,timers,created,freq_changes,updates;
static int64_t clock_us;
static void (*callback)(void *);
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *p){return p;}
static int xSemaphoreTake(SemaphoreHandle_t p,unsigned delay){
 (void)p;assert(delay==0);if(contended)return 0;assert(!held);held=1;return 1;
}
static void xSemaphoreGive(SemaphoreHandle_t p){(void)p;assert(held);held=0;}
static int64_t esp_timer_get_time(void){return clock_us;}
static int ledc_timer_config(const ledc_timer_config_t *p){
 assert(p->speed_mode==LEDC_LOW_SPEED_MODE&&p->timer_num==LEDC_TIMER_1);
 assert(p->freq_hz==CONFIG_PANEL_BUZZ_FREQUENCY_HZ&&p->duty_resolution==10);frequency=p->freq_hz;timers++;return fail_stage==1?1:0;
}
static int ledc_channel_config(const ledc_channel_config_t *p){
 assert(p->gpio_num==10&&p->channel==LEDC_CHANNEL_1&&p->timer_sel==LEDC_TIMER_1);
 assert(p->flags.output_invert==!CONFIG_DIRECT_BUZZ_ACTIVE_LEVEL&&p->duty==0);
 duty=0;return fail_stage==2?1:0;
}
static int ledc_set_freq(int mode,int timer,uint32_t hz){
 assert(held&&mode==LEDC_LOW_SPEED_MODE&&timer==LEDC_TIMER_1&&hz>=800&&hz<=8000);
 freq_changes++;if(fail_stage==7)return 1;frequency=hz;return 0;
}
static int ledc_set_duty(int mode,int channel,uint32_t value){
 assert(held&&mode==LEDC_LOW_SPEED_MODE&&channel==LEDC_CHANNEL_1&&value<=512);
 if(fail_stage==3)return 1;duty=value;return 0;
}
static int ledc_update_duty(int mode,int channel){
 assert(mode==LEDC_LOW_SPEED_MODE&&channel==LEDC_CHANNEL_1);updates++;return fail_stage==4?1:0;
}
static int ledc_stop(int mode,int channel,int idle){
 assert(mode==LEDC_LOW_SPEED_MODE&&channel==LEDC_CHANNEL_1&&idle==0);
 stops++;duty=0;return 0;
}
static int esp_timer_create(const esp_timer_create_args_t *p,esp_timer_handle_t *out){
 assert(p->callback);if(fail_stage==5)return 1;callback=p->callback;created++;*out=(void *)1;return 0;
}
static int esp_timer_start_periodic(esp_timer_handle_t h,unsigned period){
 (void)h;assert(period==5000);return fail_stage==6?1:0;
}
static int esp_timer_delete(esp_timer_handle_t h){(void)h;deleted++;return 0;}
API
SOURCE
static void reset(void){
 s_timer=NULL;s_pwm_ready=false;s_lock=NULL;s_pattern=NULL;s_note_index=0;s_deadline_us=0;
 s_output_hz=s_output_duty=0;s_applied_cancel=0;s_pending_alarm=PANEL_SOUND_NONE;s_pending_recovery=false;
 atomic_store(&s_enabled,false);atomic_store(&s_volume,100);atomic_store(&s_cancel,0);
 fail_stage=held=deleted=contended=0;duty=frequency=stops=timers=created=freq_changes=updates=0;
 clock_us=0;callback=NULL;
}
static void tick_at(int64_t us){clock_us=us;assert(callback);callback(NULL);assert(!held);}
static void ready(void){reset();assert(direct_buzzer_init()==ESP_OK);direct_buzzer_set_enabled(true);}
static unsigned expected_hz(unsigned original){
#if CONFIG_PANEL_BUZZ_FREQUENCY_HZ == 2048
 switch(original){
  case 2200: return 1718;
  case 2772: return 1861;
  case 3296: return 1992;
  case 4400: return 2268;
  case 3520: return 2048;
  case 2637: return 1828;
  case 3951: return 2155;
  case 1976: return 1662;
  case 1568: return 1560;
  case 1175: return 1462;
  case 3136: return 1952;
  case 3920: return 2148;
  case 1960: return 1658;
 }
#endif
 return original;
}
static void pattern_timing(void){
 const panel_sound_effect_t effects[]={PANEL_SOUND_BOOT,PANEL_SOUND_KEY,PANEL_SOUND_CONFIRM,
  PANEL_SOUND_BACK,PANEL_SOUND_SUCCESS,PANEL_SOUND_ERROR,PANEL_SOUND_WARNING,PANEL_SOUND_FAULT,PANEL_SOUND_RECOVERY};
 const bool single_back = CONFIG_PANEL_BUZZ_FREQUENCY_HZ == 2048;
 const unsigned count[]={7,1,3,single_back?1:3,5,3,3,8,3};
 const unsigned hz[][8]={{2200,0,2772,0,3296,0,4400},{3520},{2637,0,3951},
  {single_back?2048:2637,0,1976},{2637,0,3296,0,3951},{1568,0,1175},
  {3136,0,3136},{3920,1960,0,3920,1960,0,3920,1960},{3136,0,3951}};
 const unsigned ms[][8]={{60,25,60,25,70,25,110},{18},{35,20,55},
  {single_back?22:30,10,35},{55,20,55,20,90},{70,30,100},
  {90,100,90},{100,100,60,100,100,60,100,100},{60,20,90}};
 for(unsigned e=0;e<sizeof(effects)/sizeof(effects[0]);e++){
  ready();panel_sound_play(effects[e]);int64_t at=0;
  for(unsigned n=0;n<count[e];n++){
   assert((hz[e][n]?duty==512&&frequency==expected_hz(hz[e][n]):!duty));
   at+=(int64_t)ms[e][n]*1000;tick_at(at-1);
   assert((hz[e][n]?duty==512&&frequency==expected_hz(hz[e][n]):!duty));tick_at(at);
  }
  assert(!duty);unsigned before=updates;tick_at(at+100000);assert(!duty&&updates==before);
 }
}
int main(void){
#if CONFIG_DIRECT_BUZZ_PIN < 0
 reset();assert(direct_buzzer_init()==ESP_OK);direct_buzzer_set_enabled(true);
 panel_sound_play(PANEL_SOUND_FAULT);direct_buzzer_pulse();assert(!duty&&!created&&!timers);
 puts("PASS passive buzzer: disabled GPIO remains silent");return 0;
#elif defined(PANEL_BUZZER_DIAGNOSTIC)
 const unsigned frequencies[]={2000,2500,2700,3200,3600,4000,4500};
 reset();assert(direct_buzzer_init()==ESP_OK);
 panel_sound_play(PANEL_SOUND_BOOT);assert(!duty);
 direct_buzzer_set_enabled(true);panel_sound_play(PANEL_SOUND_BOOT);
 for(unsigned i=0;i<sizeof(frequencies)/sizeof(frequencies[0]);i++){
  int64_t start=(int64_t)i*800000;
  tick_at(start);assert(duty==512&&frequency==frequencies[i]);
  tick_at(start+399999);assert(duty==512&&frequency==frequencies[i]);
  tick_at(start+400000);assert(!duty);
 }
 tick_at(6000000);assert(!duty);
 panel_sound_key(3);assert(duty==512&&frequency==expected_hz(3520));
 ready();panel_sound_play(PANEL_SOUND_BOOT);tick_at(100000);
 panel_sound_play(PANEL_SOUND_FAULT);assert(duty==512&&frequency==expected_hz(3920));
 tick_at(820000);assert(!duty);
 puts("PASS passive buzzer diagnostic: seven equal-width 50% PWM tones, mute and fault priority");return 0;
#else
 reset();assert(direct_buzzer_init()==ESP_OK&&timers==1&&created==1);
 panel_sound_play(PANEL_SOUND_BOOT);panel_sound_key(2);direct_buzzer_pulse();assert(!duty);
 direct_buzzer_set_enabled(true);panel_sound_play(PANEL_SOUND_BOOT);assert(duty==512&&frequency==expected_hz(2200));
 assert(direct_buzzer_init()==ESP_OK&&timers==1&&created==1);
 pattern_timing();
 ready();panel_sound_play(PANEL_SOUND_BOOT);tick_at(90000);assert(duty&&frequency==expected_hz(2772));
 tick_at(160000);assert(!duty);tick_at(1000000);assert(!duty&&!s_pattern);
 ready();panel_sound_play(PANEL_SOUND_BOOT);tick_at(5000);panel_sound_play(PANEL_SOUND_KEY);
 assert(frequency==expected_hz(2200));panel_sound_play(PANEL_SOUND_WARNING);assert(frequency==expected_hz(3136));
 panel_sound_play(PANEL_SOUND_SUCCESS);assert(frequency==expected_hz(3136));panel_sound_play(PANEL_SOUND_FAULT);
 assert(frequency==expected_hz(3920));tick_at(105000);assert(frequency==expected_hz(1960));
 panel_sound_play(PANEL_SOUND_FAULT);tick_at(205000);assert(!duty);tick_at(725000);assert(!duty);
 ready();panel_sound_play(PANEL_SOUND_BOOT);contended=1;
 panel_sound_play(PANEL_SOUND_FAULT);panel_sound_play(PANEL_SOUND_WARNING);
 contended=0;tick_at(5000);assert(duty&&frequency==expected_hz(3920));
 tick_at(105000);assert(duty&&frequency==expected_hz(1960));
 ready();contended=1;panel_sound_play(PANEL_SOUND_FAULT);
 direct_buzzer_set_enabled(false);direct_buzzer_set_enabled(true);
 contended=0;tick_at(5000);assert(!duty);
 ready();panel_sound_play(PANEL_SOUND_FAULT);tick_at(300000);
 panel_sound_play(PANEL_SOUND_RECOVERY);assert(frequency==expected_hz(3920));
 tick_at(720000);assert(duty&&frequency==expected_hz(3136));tick_at(890000);assert(!duty);
 ready();panel_sound_play(PANEL_SOUND_FAULT);panel_sound_play(PANEL_SOUND_RECOVERY);
 panel_sound_play(PANEL_SOUND_WARNING);tick_at(720000);assert(!duty);
 ready();panel_sound_play(PANEL_SOUND_FAULT);panel_sound_play(PANEL_SOUND_RECOVERY);
 direct_buzzer_set_enabled(false);direct_buzzer_set_enabled(true);tick_at(720000);assert(!duty);
 ready();panel_sound_key(3);assert(frequency==expected_hz(3520));tick_at(18000);panel_sound_key(2);assert(frequency==expected_hz(2637));
 tick_at(73000);assert(frequency==expected_hz(3951));tick_at(128000);panel_sound_key(1);assert(frequency==(CONFIG_PANEL_BUZZ_FREQUENCY_HZ==2048?2048:2637));
 tick_at(203000);for(unsigned key=0;key<10;key++)if(key==0||key>=7)panel_sound_key(key);assert(!duty);
 panel_sound_play(PANEL_SOUND_NONE);panel_sound_play(PANEL_SOUND_COUNT);panel_sound_play((panel_sound_effect_t)-1);assert(!duty);
 ready();direct_buzzer_pulse();assert(duty&&frequency==CONFIG_PANEL_BUZZ_FREQUENCY_HZ);tick_at(60000);direct_buzzer_pulse();
 tick_at(70000);assert(duty);tick_at(130000);assert(!duty);direct_buzzer_set_duration_ms(120);
 direct_buzzer_pulse();tick_at(200000);assert(!duty);
 ready();panel_sound_play(PANEL_SOUND_SUCCESS);direct_buzzer_set_volume(25);assert(duty==128);
 direct_buzzer_set_volume(60);assert(duty==307);direct_buzzer_set_volume(255);assert(duty==512);
 direct_buzzer_set_enabled(false);assert(!duty&&!s_pattern);direct_buzzer_set_enabled(true);tick_at(5000);assert(!duty);
 panel_sound_play(PANEL_SOUND_KEY);direct_buzzer_set_volume(0);assert(!duty);
 direct_buzzer_set_volume(100);tick_at(6000);assert(!duty);panel_sound_play(PANEL_SOUND_KEY);assert(duty);
 contended=1;direct_buzzer_set_enabled(false);direct_buzzer_set_enabled(true);tick_at(10000);
 contended=0;tick_at(11000);assert(!duty&&!s_pattern);
 panel_sound_play(PANEL_SOUND_KEY);assert(duty);contended=1;direct_buzzer_set_volume(0);
 direct_buzzer_set_volume(100);contended=0;tick_at(12000);assert(!duty&&!s_pattern);
 ready();clock_us=((int64_t)UINT32_MAX+12345)*1000;panel_sound_play(PANEL_SOUND_KEY);
 assert(duty);tick_at(clock_us+18000);assert(!duty);
 for(int stage=1;stage<=7;stage++){
  reset();fail_stage=stage;
  if(stage==3||stage==4||stage==7){assert(direct_buzzer_init()==ESP_OK);direct_buzzer_set_enabled(true);panel_sound_play(PANEL_SOUND_BOOT);assert(!duty&&!s_pwm_ready);}
  else assert(direct_buzzer_init()!=ESP_OK&&!duty);
  assert(!held);unsigned previous_created=created;fail_stage=0;
  assert(direct_buzzer_init()==ESP_OK);direct_buzzer_set_enabled(true);panel_sound_play(PANEL_SOUND_KEY);assert(duty);
  if(previous_created&&stage!=6)assert(created==previous_created);
 }
 puts("PASS passive buzzer: nine timed effects, late ticks, priority, no restart, startup mute, atomic mute/volume, 70ms factory pulse, PWM isolation, polarity and failure retry");
 return 0;
#endif
}
'''.replace('API', api).replace('SOURCE', body)
source = root/'tmp/buzzer_runtime_test.c'
source.write_text(test, encoding='utf-8')
exe = source.with_suffix('.exe')
for pin, polarity, rated_hz, diagnostic in ((10,0,4000,False),(10,1,4000,False),(10,1,2048,False),
                                           (-1,1,4000,False),(10,1,4000,True)):
    mode_flags = ['-DPANEL_BUZZER_DIAGNOSTIC=1'] if diagnostic else []
    subprocess.run([sys.argv[1], '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-misleading-indentation',
                    '-Wno-unused-function', f'-DCONFIG_DIRECT_BUZZ_PIN={pin}',
                    f'-DCONFIG_DIRECT_BUZZ_ACTIVE_LEVEL={polarity}',
                    f'-DCONFIG_PANEL_BUZZ_FREQUENCY_HZ={rated_hz}', *mode_flags,
                    str(source), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
