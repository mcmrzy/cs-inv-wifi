"""Exercise the real USB observation formatter with missing, zero and stale data."""
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'main/factory/factory_service.c').read_text(encoding='utf-8')
match = re.search(r'^static void factory_print_dsp_observation\(void\)\s*\{', source, re.M)
assert match, 'USB status must expose independent DSP observations'
brace = source.index('{', match.start())
end, depth = brace + 1, 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
code = r'''
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "direct_link.h"
static bool available;
static char output[8192];static size_t used;
static int capture(const char *format,...){va_list args;va_start(args,format);int n=vsnprintf(output+used,sizeof(output)-used,format,args);va_end(args);assert(n>=0 && (size_t)n<sizeof(output)-used);used+=(size_t)n;return n;}
#define printf capture
static bool direct_config_observed_snapshot_wire_stub(direct_config_observation_t *o){
 if(!available)return false;memset(o,0,sizeof(*o));o->sequence=3;o->received_ms=100;o->age_ms=6900;o->valid[7]=o->valid[40]=true;o->values[7]=2200;return true;
}
static bool direct_dsp_get_status_sample_stub(direct_dsp_status_t *s,uint32_t *at){
 if(!available)return false;memset(s,0,sizeof(*s));s->inverter_status[0]=0x0200;s->inverter_status[1]=0x0080;*at=6000;return true;
}
#define direct_config_observed_snapshot_wire direct_config_observed_snapshot_wire_stub
#define direct_dsp_get_status_sample direct_dsp_get_status_sample_stub
SOURCE
int main(void){
 factory_print_dsp_observation();assert(strstr(output,"[DSP_OBSERVED] available=0") && !strstr(output,"DSP_OBSERVED_PARAM") && !strstr(output,"DSP_STATUS_RAW"));
 used=0;output[0]=0;available=true;factory_print_dsp_observation();
 assert(strstr(output,"source=25") && strstr(output,"seq=3") && strstr(output,"age_ms=6900"));
 assert(strstr(output,"index=7 wire_raw=2200") && strstr(output,"index=40 wire_raw=0"));
 assert(!strstr(output,"index=41 ") && !strstr(output,"applied") && !strstr(output,"confirmed"));
 assert(strstr(output,"[DSP_STATUS_RAW]") && strstr(output,"status0=0x0200") && strstr(output,"status1=0x0080"));
 puts("PASS actual USB status: no fabricated observation, known zero, unknown omitted, stale age, raw status bits, no applied claim");
}
'''.replace('SOURCE', source[match.start():end])
target = ROOT / 'tmp/dsp_observation_status_test.c'
target.write_text(code, encoding='utf-8')
gcc = r'C:\Users\29538\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe'
exe = target.with_suffix('.exe')
subprocess.run([gcc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-misleading-indentation', '-Imain/direct', '-Imain/telemetry', '-Imain/ota', str(target), '-o', str(exe)], cwd=ROOT, check=True)
subprocess.run([str(exe)], check=True)
