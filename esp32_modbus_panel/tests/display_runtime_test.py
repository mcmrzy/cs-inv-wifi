"""Run actual LCD wake/sleep/save helpers without SPI or a physical panel."""
from pathlib import Path
import subprocess
import sys
root=Path(__file__).resolve().parents[1]
source=(root/'full/full_panel_runtime.inc').read_text(encoding='utf-8')
helpers=source[source.index('static void panel_set_blank('):source.index('static void panel_apply_preferences(')]
feedback=source[source.index('static bool panel_alarm_feedback('):source.index('static void panel_set_blank(')]
normal=source[source.index('bool woke=panel_wake_key();'):]
assert normal.index('if(woke)continue;')<normal.index('direct_ui_key(')
modbus=source[source.index('static void full_modbus_panel_tick('):source.index('static void panel_task(')]
assert modbus.index('if(panel_wake_key())continue;')<modbus.index('panel_ui_key(')
assert 's_preview_display=s_saved_display;' in source
assert source.count('panel_alarm_feedback(&previous_alarm)')==2
test=r'''
#include "panel_display_settings.h"
#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
static bool s_blank,s_have_previous=true,s_modbus_drawn=true;
static uint32_t s_last_input,clock_ms;
static uint8_t s_saved_style=2,s_preview_style=2;
static uint16_t s_local_blank=1;
static bool s_ui_data;static uint16_t s_local_alarm=1;
static unsigned beeps;
static bool direct_ui_has_alarm(const bool *d){return *d;}
static void panel_beep(void){beeps++;}
static panel_display_settings_t s_saved_display={80,2},s_preview_display={80,2};
static unsigned commands;static uint8_t last_command,brightness;static bool backlight_off;
static uint32_t now_ms(void){return clock_ms;}
static void command(uint8_t c){last_command=c;commands++;}
static void full_backlight_set(uint8_t b,bool off){brightness=b;backlight_off=off;}
''' + feedback + helpers + r'''
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
 bool previous=false;
 clock_ms=122000;s_ui_data=true;panel_idle_tick(panel_alarm_feedback(&previous));
 assert(!s_blank && !backlight_off && brightness==80 && s_last_input==122000 && beeps==1);
 assert(panel_alarm_feedback(&previous) && beeps==1);
 s_ui_data=false;assert(!panel_alarm_feedback(&previous));s_ui_data=true;
 s_local_alarm=0;assert(panel_alarm_feedback(&previous) && beeps==1);
 clock_ms=122001;panel_idle_tick(false);assert(!s_blank);
 s_preview_display.timeout_index=0;clock_ms=999999;panel_idle_tick(false);assert(!s_blank);
 assert(panel_key_current(clock_ms));assert(panel_key_current(clock_ms-1000));assert(!panel_key_current(clock_ms-1001));
 clock_ms=5;assert(panel_key_current(UINT32_MAX-10));assert(!panel_key_current(UINT32_MAX-2000));
 puts("PASS display runtime: first-key wake, alarm/beep edges, brightness/cancel, never and old-event rejection across uptime wrap");return 0;
}
'''
path=root/'tmp/display_runtime_test.c';path.write_text(test,encoding='utf-8')
for pin in (5,-1):
    exe=root/f'tmp/display_runtime_test_{pin}.exe'
    subprocess.run([sys.argv[1],'-save-temps=obj','-std=c11','-Wall','-Wextra','-Werror',f'-DCONFIG_DIRECT_LCD_BL_PIN={pin}','-I'+str(root/'main'),str(path),str(root/'main/panel_display_settings.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
