"""Verify actual config JSON uses controller settings and omits unavailable fields."""
from pathlib import Path
import re, subprocess, json
root=Path(__file__).resolve().parents[1]
def function(source,name):
    m=re.search(r'^(?:static )?(?:void|uint16_t) '+name+r'\([^;]*?\)\s*\{',source,re.M);assert m
    begin=source.index('{',m.start());end=begin+1;depth=1
    while depth:depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[m.start():end]
tele=(root/'main/telemetry/telemetry.c').read_text(encoding='utf-8')
poll=(root/'main/direct/direct_poll.c').read_text(encoding='utf-8')
start=tele.index('static const cfg_key_t s_cfg_keys');end=tele.index('};',start)+2
code=r'''
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "direct_dsp_params.h"
#define CTRL_PARAM_COUNT 42
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define TOPIC_CONFIG "config"
typedef struct {const char *name;uint8_t scale;} cfg_key_t;
TABLE
COLLECTOR
static direct_dsp_params_t params;
static bool direct_config_get_wire(uint16_t index,uint16_t *value){uint16_t native;if(!direct_dsp_params_get(&params,index,&native))return false;*value=collector_value(index,native);return true;}
static int mqtt_publish(const char *topic,const char *buf,int n,int qos,int retain){(void)n;assert(!strcmp(topic,"config") && qos==1 && retain==0);puts(buf);return 0;}
PUBLISH
int main(void){assert(direct_dsp_params_arm_defaults(&params));assert(direct_dsp_params_set(&params,5,0));uint16_t values[42]={0};bool valid[42];for(unsigned i=0;i<42;i++)valid[i]=direct_config_get_wire((uint16_t)i,&values[i]);publish_config(values,valid,2);}
'''.replace('TABLE',tele[start:end]).replace('COLLECTOR',function(poll,'collector_value')).replace('PUBLISH',function(tele,'publish_config'))
target=root/'tmp/controller_config_publish_test.c';target.write_text(code,encoding='utf-8')
gcc=Path(r'C:\Users\29538\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe')
exe=root/'tmp/controller_config_publish_test.exe'
subprocess.run([str(gcc),'-std=c11','-Wall','-Wextra','-Werror','-Imain/direct','-Imain/telemetry',str(target),'main/direct/direct_protocol.c','main/direct/direct_dsp_params.c','-o',str(exe)],cwd=root,check=True)
result=json.loads(subprocess.check_output([str(exe)],text=True));p=result['params']
assert result['rev']==2 and p['set_output_voltage']==230 and p['set_output_frequency']==50
assert p['set_max_charge_current']==140 and p['set_ac_charge_current']==60
assert p['set_low_volt_return_utl']==48 and p['set_high_volt_return_bat']==57.6
assert p['set_high_temp_restart']==1 and p['set_overload_restart']==0
assert 'set_battery_capacity' not in p and 'set_max_discharge_current' not in p
print('PASS actual controller config JSON: engineering units, saved zero remains zero, unknown fields omitted, no fabricated observed settings')
