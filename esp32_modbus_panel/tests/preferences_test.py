"""Run the actual preferences store against atomic NVS/lock fault fixtures."""
from pathlib import Path
import subprocess
import sys
root=Path(__file__).resolve().parents[1]
body=(root/'full/panel_preferences.c').read_text(encoding='utf-8')
body=body.replace('#include "nvs.h"','').replace('#include "freertos/FreeRTOS.h"','').replace('#include "freertos/semphr.h"','')
test=r'''
#include "panel_preferences.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef int nvs_handle_t;typedef int esp_err_t;typedef int StaticSemaphore_t;
typedef int *SemaphoreHandle_t;typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(x) ((void)(x))
#define taskEXIT_CRITICAL(x) ((void)(x))
#define pdMS_TO_TICKS(x) (x)
#define pdTRUE 1
enum {ESP_OK=0,NVS_READONLY,NVS_READWRITE,ESP_ERR_NVS_NOT_FOUND,ESP_ERR_NVS_TYPE_MISMATCH};
static int held,commit_fail,set_fail,commits,read_fail;
static bool have_blob,legacy;
static unsigned char flash_blob[16],pending_blob[16];
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s){return s;}
static int xSemaphoreTake(SemaphoreHandle_t s,int timeout){(void)s;(void)timeout;assert(!held);held=1;return 1;}
static void xSemaphoreGive(SemaphoreHandle_t s){(void)s;assert(held);held=0;}
static int nvs_open(const char *name,int mode,nvs_handle_t *out){(void)mode;assert(!strcmp(name,"direct_ui"));*out=1;return 0;}
static int nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *size){(void)h;assert(!strcmp(key,"prefs"));if(read_fail)return 99;if(!have_blob)return ESP_ERR_NVS_NOT_FOUND;assert(*size==16);memcpy(out,flash_blob,16);return 0;}
static int nvs_set_blob(nvs_handle_t h,const char *key,const void *value,size_t size){(void)h;assert(!strcmp(key,"prefs")&&size==16);if(set_fail)return 1;memcpy(pending_blob,value,16);return 0;}
static int nvs_commit(nvs_handle_t h){(void)h;commits++;if(commit_fail)return 1;memcpy(flash_blob,pending_blob,16);have_blob=true;return 0;}
static void nvs_close(nvs_handle_t h){(void)h;}
static int nvs_get_u16(nvs_handle_t h,const char *k,uint16_t *v){(void)h;if(!legacy)return ESP_ERR_NVS_NOT_FOUND;*v=strcmp(k,"blank")?1:0;return 0;}
static int nvs_get_i16(nvs_handle_t h,const char *k,int16_t *v){(void)h;(void)k;*v=480;return legacy?0:ESP_ERR_NVS_NOT_FOUND;}
static int nvs_get_u8(nvs_handle_t h,const char *k,uint8_t *v){(void)h;(void)k;*v=2;return legacy?0:ESP_ERR_NVS_NOT_FOUND;}
static int nvs_get_u32(nvs_handle_t h,const char *k,uint32_t *v){(void)h;(void)k;*v=0xb1050000U|((uint32_t)panel_display_pack((panel_display_settings_t){60,3})<<2)|2;return legacy?0:ESP_ERR_NVS_NOT_FOUND;}
SOURCE
int main(void){
 panel_preferences_t a,b;assert(panel_preferences_get(&a));
 assert(a.style==2&&a.display.brightness==100&&a.blank==1&&a.timezone==480);
 assert(a.feedback.led_brightness==24&&a.feedback.beep_mode==PANEL_BEEP_HIGH);
 a.display=(panel_display_settings_t){50,4};a.style=1;
 assert(panel_preferences_update(&a,PANEL_PREF_DISPLAY));
 assert(panel_preferences_get(&b)&&b.style==1&&b.display.brightness==50);
 /* Phone's stale snapshot updates local flags without reverting appearance. */
 a=defaults;a.blank=0;a.buzzer=0;assert(panel_preferences_update(&a,PANEL_PREF_LOCAL));
 assert(panel_preferences_get(&b)&&b.style==1&&b.display.brightness==50&&b.blank==0&&!b.buzzer);
 /* Time update merges current local flags and appearance. */
 a=defaults;a.timezone=-300;assert(panel_preferences_update(&a,PANEL_PREF_TIME));
 assert(panel_preferences_get(&b)&&b.timezone==-300&&b.blank==0&&b.style==1);
 /* A stale single-parameter remote write cannot revert a new display timeout. */
 a=defaults;a.display.timeout_index=0;assert(panel_preferences_update(&a,PANEL_PREF_DISPLAY));
 a=defaults;a.alarm=0;assert(panel_preferences_update(&a,PANEL_PREF_ALARM));
 assert(panel_preferences_get(&b)&&!b.alarm&&!b.blank&&b.display.timeout_index==0&&!b.buzzer);
 a=defaults;a.style=1;a.display.brightness=50;assert(panel_preferences_update(&a,PANEL_PREF_DISPLAY));
 a=defaults;a.blank=0;assert(panel_preferences_update(&a,PANEL_PREF_BLANK));
 assert(panel_preferences_get(&b)&&!b.alarm&&!b.blank&&!b.buzzer&&b.style==1&&b.timezone==-300);
 /* A phone slider updates only the value it edited. */
 panel_preferences_t brightness={.display.brightness=65};
 assert(panel_preferences_update(&brightness,PANEL_PREF_BRIGHTNESS));
 assert(panel_preferences_get(&b)&&b.style==1&&b.display.brightness==65&&!b.blank&&b.timezone==-300);
 panel_preferences_t language={.style=2};
 assert(panel_preferences_update(&language,PANEL_PREF_LANGUAGE));
 assert(panel_preferences_get(&b)&&b.style==3&&b.display.brightness==65&&!b.blank);
 a=defaults;a.style=1;a.display.brightness=50;assert(panel_preferences_update(&a,PANEL_PREF_DISPLAY));
 unsigned char before[16];memcpy(before,flash_blob,16);
 commit_fail=1;assert(!panel_preferences_reset());
 assert(!memcmp(before,flash_blob,16));assert(panel_preferences_get(&a)&&a.timezone==-300&&a.style==1);
 commit_fail=0;set_fail=1;assert(!panel_preferences_reset());set_fail=0;
 a.feedback=(panel_feedback_settings_t){0,PANEL_BEEP_LONG};a.buzzer=0;
 assert(panel_preferences_update(&a,PANEL_PREF_LED_BRIGHTNESS|PANEL_PREF_BEEP_MODE));
 loaded=false;assert(panel_preferences_get(&a)&&a.feedback.led_brightness==0&&!a.buzzer&&a.feedback.beep_mode==PANEL_BEEP_LONG);
 b=defaults;b.timezone=0;assert(panel_preferences_update(&b,PANEL_PREF_TIME));
 assert(panel_preferences_get(&a)&&a.feedback.led_brightness==0&&!a.buzzer&&a.feedback.beep_mode==PANEL_BEEP_LONG);
 assert(panel_preferences_reset());assert(panel_preferences_get(&a)&&a.style==2&&a.blank==1&&a.buzzer==1);
 int count=commits;assert(panel_preferences_reset()&&commits==count);
 loaded=false;read_fail=1;assert(!panel_preferences_get(&a)&&!panel_preferences_reset()&&commits==count);
 read_fail=0;flash_blob[2]=2;assert(!panel_preferences_reset()&&commits==count);flash_blob[2]=1;
 /* Reboot uses committed blob even when old scalar keys conflict. */
 legacy=true;loaded=false;assert(panel_preferences_get(&a)&&a.blank==1&&a.display.brightness==100);
 /* Existing v1 records have no feedback extension. Retain their backlight. */
 flash_blob[10]=80;flash_blob[12]=flash_blob[13]=flash_blob[14]=flash_blob[15]=0;loaded=false;
 assert(panel_preferences_get(&a)&&a.display.brightness==80&&a.feedback.led_brightness==24&&a.feedback.beep_mode==PANEL_BEEP_HIGH);
 /* Duration-era preferences retain their electrical volume and explicit OFF. */
 flash_blob[5]=0;flash_blob[12]=17;
 for(unsigned old_mode=1;old_mode<=3;old_mode++){
  flash_blob[13]=(unsigned char)old_mode;loaded=false;
  assert(panel_preferences_get(&a)&&!a.buzzer&&a.feedback.led_brightness==17&&a.feedback.beep_mode==PANEL_BEEP_HIGH);
 }
 a.feedback.beep_mode=PANEL_BEEP_LOW;
 assert(panel_preferences_update(&a,PANEL_PREF_BEEP_MODE));
 assert(flash_blob[13]==2&&flash_blob[14]==PANEL_BEEP_LOW&&flash_blob[15]==0xb2);
 loaded=false;assert(panel_preferences_get(&a)&&!a.buzzer&&a.feedback.beep_mode==PANEL_BEEP_LOW);
 /* Legacy OFF survives migration despite a nonzero saved screen timeout. */
 have_blob=false;loaded=false;assert(panel_preferences_get(&a)&&a.blank==0&&a.display.brightness==60);
 a.timezone=9999;assert(!panel_preferences_update(&a,PANEL_PREF_TIME));
 assert(panel_preferences_get(&b)&&b.timezone==480);
 have_blob=true;memset(flash_blob,255,16);loaded=false;
 assert(panel_preferences_get(&a)&&a.blank==0&&a.display.brightness==60);
 assert(!held);puts("PASS preferences: atomic commit/rollback, concurrent field merge, reset, reboot, legacy OFF migration and corrupt record");
}
'''.replace('SOURCE',body)
path=root/'tmp/preferences_test.c';path.write_text(test,encoding='utf-8')
exe=path.with_suffix('.exe')
subprocess.run([sys.argv[1],'-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'full'),'-I'+str(root/'main'),str(path),str(root/'main/panel_display_settings.c'),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
