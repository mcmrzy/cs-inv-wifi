"""Compare firmware defaults against the supplied ARM initialization and encoders."""
from pathlib import Path
import re, subprocess
root=Path(__file__).resolve().parents[1]
source=Path(r'C:\Users\29538\Documents\xwechat_files\wxid_6n4stl4q5dzq22_9518\msg\file\2026-09\App(10)\App\source')
user=(source/'USERFLASH/USERFLASH.c').read_text(encoding='utf-8',errors='replace')
comm=(source/'ComDSP/UsartDsp.c').read_text(encoding='utf-8',errors='replace')
config=(source/'APP/SystemConfig.c').read_text(encoding='utf-8',errors='replace')
def function(text,name):
    match=re.search(r'^void '+name+r'\([^;]*?\)\s*\{',text,re.M)
    assert match,name
    at=text.index('{',match.start());end=at+1;depth=1
    while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[match.start():end]
initialize=function(user,'init_setting_param_L00F960_flash')
encoders='\n'.join(function(comm,f'fill_cmd0{i}_packet') for i in range(3))
fields=sorted(set(re.findall(r'inverterParam1\.(\w+)',initialize+encoders))-{'configWord','configWord1'})
generator={name:int(re.search(r'SysParam\.'+name+r'\s*=\s*(\d+)\s*;',config).group(1))
           for name in ('GeStartSOC','GeStopSOC','GeStartVoltage','GeStopVoltage','GeRatedPower')}
assert 'memset((u8*)&SysParam,0,sizeof(SysParam))' in user.replace(' ','')
code='''#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "direct_dsp_params.h"
typedef uint8_t u8;typedef uint16_t u16;
#define USART1 1
struct {FIELDS struct {uint16_t _word;} configWord,configWord1;} inverterParam1;
struct {uint16_t GeBalanceEn,GeStartSOC,GeStopSOC,GeStartVoltage,GeStopVoltage,GeRatedPower;} SysParam;
static uint8_t captured[64];static size_t used;
static uint16_t calculate_CRCmodbus16(uint8_t *b,size_t n){return direct_crc16(b,n);}
static void USART_SendDataPackage(int port,uint8_t *b,size_t n){assert(port==1 && used+n<=64);memcpy(captured+used,b,n);used+=n;}
INITIALIZE
ENCODERS
int main(void){
 init_setting_param_L00F960_flash();GENERATOR
 uint8_t out[25];fill_cmd00_packet(out);fill_cmd01_packet(out);fill_cmd02_packet(out);
 direct_dsp_params_t actual;assert(direct_dsp_params_arm_defaults(&actual));
 assert(used==64 && !memcmp(captured,actual.frames,64));
 puts("PASS original ARM defaults: actual initialization and three encoders match all 64 bytes; generator disabled, 3000W");
}
'''.replace('FIELDS',''.join('uint16_t '+name+';' for name in fields)).replace('INITIALIZE',initialize).replace('ENCODERS',encoders)
code=code.replace('GENERATOR',''.join(f'SysParam.{name}={value};' for name,value in generator.items()))
target=root/'tmp/dsp_defaults_original.c';target.write_text(code,encoding='utf-8')
gcc=Path(r'C:\Users\29538\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe')
exe=root/'tmp/dsp_defaults_original.exe'
subprocess.run([str(gcc),'-std=c11','-Wall','-Wextra','-Werror','-Imain/direct','-Imain/telemetry',str(target),'main/direct/direct_protocol.c','main/direct/direct_dsp_params.c','main/direct/direct_battery_config.c','-o',str(exe)],cwd=root,check=True)
subprocess.run([str(exe)],check=True)
poll=(root/'main/direct/direct_poll.c').read_text(encoding='utf-8')
init=function(poll,'arm_poll_init')
assert init.index('direct_dsp_params_bootstrap(')<init.index('xTaskCreate(dsp_task,')
assert 's_dsp_sync_pending=s_dsp_params_valid?(direct_dsp_params_charge_reply(&s_dsp_params,charge)?15U:7U):0U;' in init
