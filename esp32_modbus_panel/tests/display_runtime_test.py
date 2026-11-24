"""Run actual LCD wake/sleep/save helpers without SPI or a physical panel."""
from pathlib import Path
import subprocess
import sys
root=Path(__file__).resolve().parents[1]
source=(root/'full/full_panel_runtime.inc').read_text(encoding='utf-8')
helpers=source[source.index('static void panel_set_blank('):source.index('static void panel_apply_preferences(')]
feedback=source[source.index('static bool panel_alarm_feedback('):source.index('static void panel_set_blank(')]
identity=source[source.index('static void panel_snapshot_identity('):source.index('static void panel_snapshot(')]
assert 'panel_snapshot_identity(sampled_ms);' in source
normal=source[source.index('bool woke=panel_wake_key();'):]
assert normal.index('if(woke)continue;')<normal.index('direct_ui_key(')
modbus=source[source.index('static void full_modbus_panel_tick('):source.index('static void panel_task(')]
assert modbus.index('if(panel_wake_key())continue;')<modbus.index('panel_ui_key(')
assert 's_preview_display=s_saved_display;' in source
assert source.count('panel_alarm_feedback()')==2
test=r'''
#include "panel_display_settings.h"
#include "panel_sound_feedback.h"
#include "factory_identity.h"
#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
static bool s_blank,s_have_previous=true,s_modbus_drawn=true;
static uint32_t s_last_input,clock_ms;
static uint8_t s_saved_style=2,s_preview_style=2;
static uint16_t s_local_blank=1;
typedef struct {
 bool bms_online;struct {uint16_t warning,protection;} bms;
 const char *model,*hardware,*sn;
} ui_data_t;
static ui_data_t s_ui_data;
static factory_record_t s_identity,stored_identity;
static uint32_t identity_revision;
static unsigned identity_reads;
static factory_result_t stored_result=FACTORY_UNPROVISIONED;
static bool change_during_read;
uint32_t factory_identity_revision(void){return identity_revision;}
factory_result_t factory_identity_read(factory_record_t *out){
 identity_reads++;
 *out=stored_identity;
 if(change_during_read){
  change_during_read=false;identity_revision++;
  strcpy(stored_identity.model,"12KU");
 }
 return stored_result;
}
static uint16_t s_local_alarm=1,s_local_buzzer=1;
static panel_alarm_sample_t sample={.fresh=15,.reachable=true};
static unsigned beeps;
static panel_sound_effect_t last_sound;
static bool direct_ui_has_alarm(const ui_data_t *d){
 return sample.words[0] || sample.words[1] || sample.words[2] || sample.words[3] ||
    (d->bms_online && (d->bms.warning || d->bms.protection));
}
static bool panel_bus_alarm_sample(panel_alarm_sample_t *out){*out=sample;return true;}
void panel_sound_play(panel_sound_effect_t effect){
 if(effect!=PANEL_SOUND_NONE){last_sound=effect;beeps++;}
}
static panel_display_settings_t s_saved_display={80,2},s_preview_display={80,2};
static unsigned commands;static uint8_t last_command,brightness;static bool backlight_off;
static uint32_t now_ms(void){return clock_ms;}
static void command(uint8_t c){last_command=c;commands++;}
static void full_backlight_set(uint8_t b,bool off){brightness=b;backlight_off=off;}
''' + feedback + helpers + identity + r'''
static void sample_identity(uint32_t now){
 memset(&s_ui_data,0,sizeof(s_ui_data));s_ui_data.sn="BOOT-SN";
 panel_snapshot_identity(now);
}
static void test_identity(void){
 for(uint32_t now=0;now<10000;now+=20){
  sample_identity(now);assert(!strcmp(s_ui_data.hardware,"UNSET"));
 }
 assert(identity_reads==1);
 strcpy(stored_identity.sn,"FACTORY-SN");strcpy(stored_identity.model,"6KU");
 strcpy(stored_identity.hw_controller,"PANEL-V2");
 stored_result=FACTORY_OK;identity_revision++;
 for(uint32_t now=10000;now<20000;now+=20){
  sample_identity(now);
  assert(!strcmp(s_ui_data.hardware,"PANEL-V2"));
  assert(!strcmp(s_ui_data.model,"6KU") && !strcmp(s_ui_data.hardware,"PANEL-V2"));
  assert(!strcmp(s_ui_data.sn,CONFIG_DIRECT_IDENTITY_PRODUCTION?"FACTORY-SN":"BOOT-SN"));
 }
 assert(identity_reads==2);
 stored_identity.locked=true;identity_revision++;
 sample_identity(20000);assert(identity_reads==3 && !strcmp(s_ui_data.hardware,"PANEL-V2"));
 stored_result=FACTORY_BUSY;identity_revision++;
 sample_identity(UINT32_MAX-100U);assert(identity_reads==4 && !strcmp(s_ui_data.hardware,"UNSET"));
 stored_result=FACTORY_OK;
 sample_identity(50);assert(identity_reads==4 && !strcmp(s_ui_data.hardware,"UNSET"));
 sample_identity(150);assert(identity_reads==5 && !strcmp(s_ui_data.hardware,"PANEL-V2"));
 identity_revision++;stored_result=FACTORY_STORAGE_ERROR;
 sample_identity(200);assert(!strcmp(s_ui_data.hardware,"UNSET") && identity_reads==6);
 stored_result=FACTORY_OK;identity_revision++;
 sample_identity(210);assert(strcmp(s_ui_data.hardware,"UNSET") && identity_reads==7);
 identity_revision++;change_during_read=true;
 sample_identity(220);assert(identity_reads==8 && !strcmp(s_ui_data.hardware,"UNSET"));
 sample_identity(220);assert(identity_reads==9 && !strcmp(s_ui_data.model,"12KU"));
 puts("PASS display identity: one storage read per stable revision, provision/lock refresh, transient retry and revision race");
}
int main(void){
 s_preview_style=0;s_preview_display.brightness=20;panel_discard_preview();
 assert(s_preview_style==2 && s_preview_display.brightness==80 && brightness==80);
 s_preview_display.brightness=PANEL_DISPLAY_DEFAULT_BRIGHTNESS;
 clock_ms=59999;panel_idle_tick(false);assert(!s_blank);
 clock_ms=60000;panel_idle_tick(false);assert(s_blank && backlight_off);
 if(CONFIG_DIRECT_LCD_BL_PIN>=0)assert(commands==0);
 else assert(commands==1 && last_command==0x28);
 clock_ms=61000;assert(panel_wake_key());assert(!s_blank && !backlight_off && brightness==100);
 if(CONFIG_DIRECT_LCD_BL_PIN>=0)assert(commands==0);
 else assert(commands==2 && last_command==0x29);
 assert(!s_have_previous && !s_modbus_drawn && s_last_input==61000);
 assert(!panel_wake_key());
 s_preview_display.brightness=80;
 clock_ms=121000;panel_idle_tick(false);assert(s_blank);
 clock_ms=122000;sample.words[2]=1;
 panel_idle_tick(panel_alarm_feedback());
 assert(!s_blank && !backlight_off && brightness==80 && s_last_input==122000);
 assert(beeps==1 && last_sound==PANEL_SOUND_WARNING);
 assert(panel_alarm_feedback() && beeps==1);
 sample.words[0]=1;
 assert(panel_alarm_feedback() && beeps==2 && last_sound==PANEL_SOUND_FAULT);
 sample.words[0]=3;
 assert(panel_alarm_feedback() && beeps==3 && last_sound==PANEL_SOUND_FAULT);
 s_ui_data.bms_online=true;s_ui_data.bms.protection=1;
 assert(panel_alarm_feedback() && beeps==4 && last_sound==PANEL_SOUND_FAULT);
 sample.words[0]=0;sample.words[2]=0;sample.paused=true;s_ui_data.bms_online=false;
 assert(!panel_alarm_feedback() && beeps==4);
 sample.paused=false;sample.fresh=14;
 assert(!panel_alarm_feedback() && beeps==4);
 sample.fresh=15;
 assert(!panel_alarm_feedback() && beeps==4);
 s_ui_data.bms_online=true;s_ui_data.bms.protection=0;
 assert(!panel_alarm_feedback() && beeps==5 && last_sound==PANEL_SOUND_RECOVERY);
 assert(!panel_alarm_feedback() && beeps==5);
 s_local_alarm=0;sample.words[2]=1;
 assert(panel_alarm_feedback() && beeps==5);
 s_local_alarm=1;
 assert(panel_alarm_feedback() && beeps==5);
 sample.words[2]=3;
 assert(panel_alarm_feedback() && beeps==6 && last_sound==PANEL_SOUND_WARNING);
 sample.words[2]=0;assert(!panel_alarm_feedback() && beeps==7);
 s_ui_data.bms.protection=1;assert(panel_alarm_feedback() && beeps==8);
 s_ui_data.bms.protection=0;sample.reachable=false;
 assert(!panel_alarm_feedback() && beeps==8);
 sample.reachable=true;sample.fresh=7;
 assert(!panel_alarm_feedback() && beeps==8);
 sample.fresh=15;
 assert(!panel_alarm_feedback() && beeps==9 && last_sound==PANEL_SOUND_RECOVERY);
 clock_ms=122001;panel_idle_tick(false);assert(!s_blank);
 s_preview_display.timeout_index=0;clock_ms=999999;panel_idle_tick(false);assert(!s_blank);
 assert(panel_key_current(clock_ms));assert(panel_key_current(clock_ms-1000));assert(!panel_key_current(clock_ms-1001));
 clock_ms=5;assert(panel_key_current(UINT32_MAX-10));assert(!panel_key_current(UINT32_MAX-2000));
 test_identity();
 puts("PASS display runtime: first-key wake, alarm/beep edges, brightness/cancel, never and old-event rejection across uptime wrap");return 0;
}
'''
path=root/'tmp/display_runtime_test.c';path.write_text(test,encoding='utf-8')
for pin in (5,-1):
    exe=root/f'tmp/display_runtime_test_{pin}.exe'
    subprocess.run([sys.argv[1],'-save-temps=obj','-std=c11','-Wall','-Wextra','-Werror',f'-DCONFIG_DIRECT_LCD_BL_PIN={pin}',f'-DCONFIG_DIRECT_IDENTITY_PRODUCTION={int(pin>=0)}','-I'+str(root/'main'),'-I'+str(root/'full'),'-I'+str(root.parent/'esp32_direct/main/factory'),str(path),str(root/'full/panel_sound_feedback.c'),str(root/'main/panel_display_settings.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
