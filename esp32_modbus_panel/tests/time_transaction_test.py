"""Exercise time-save rollback, without requiring the workstation clock to change."""
from pathlib import Path
import subprocess
import sys
root=Path(__file__).resolve().parents[1]
s=(root/'full/full_panel_runtime.inc').read_text(encoding='utf-8')
body=s[s.index('static bool panel_set_time('):s.index('static void panel_save_appearance(')]
test=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef long long time_t;
struct timeval {time_t tv_sec;int tv_usec;};
enum {PANEL_PREF_TIME=16};
static struct {int time_fields[7];} s_ui;
static int16_t s_timezone=480;
static struct timeval system_time={12345,678},saved_before;
static int fail_get,fail_set,fail_save,calls;
static bool direct_ui_time_utc(const int *f,int64_t *out){if(f[0]==0)return false;*out=99999;return true;}
static int gettimeofday(struct timeval *out,void *tz){(void)tz;if(fail_get)return -1;*out=system_time;return 0;}
static int settimeofday(const struct timeval *value,void *tz){(void)tz;calls++;if(fail_set)return -1;system_time=*value;return 0;}
static bool panel_save_local(unsigned fields){assert(fields==PANEL_PREF_TIME);return !fail_save;}
SOURCE
int main(void){
 s_ui.time_fields[0]=2026;s_ui.time_fields[6]=-300;
 saved_before=system_time;fail_save=1;assert(!panel_set_time(true));
 assert(s_timezone==480&&system_time.tv_sec==saved_before.tv_sec&&system_time.tv_usec==saved_before.tv_usec&&calls==2);
 fail_save=0;fail_set=1;assert(!panel_set_time(true)&&s_timezone==480);
 fail_set=0;fail_get=1;calls=0;assert(!panel_set_time(true)&&calls==0);
 fail_get=0;assert(panel_set_time(true)&&s_timezone==-300&&system_time.tv_sec==99999);
 calls=0;s_ui.time_fields[0]=0;assert(!panel_set_time(true)&&calls==0);
 s_ui.time_fields[6]=345;fail_get=1;fail_set=1;
 assert(panel_set_time(false)&&calls==0&&s_timezone==345&&system_time.tv_sec==99999);
 fail_save=1;s_ui.time_fields[6]=480;
 assert(!panel_set_time(false)&&calls==0&&s_timezone==345&&system_time.tv_sec==99999);
 puts("PASS time transaction: invalid/read/set/commit failures preserve timezone and wall-clock, successful save applies both");
}
'''.replace('SOURCE',body)
path=root/'tmp/time_transaction_test.c';path.write_text(test,encoding='utf-8');exe=path.with_suffix('.exe')
subprocess.run([sys.argv[1],'-std=c11','-Wall','-Wextra','-Werror',str(path),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
