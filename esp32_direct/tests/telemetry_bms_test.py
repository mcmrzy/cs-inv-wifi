"""Run the actual heartbeat builder and tick; inspect emitted JSON independently."""
from pathlib import Path
import json
import subprocess

ROOT = Path(__file__).resolve().parents[1]
GCC = Path(r'C:\Users\29538\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe')
source = (ROOT / 'main/telemetry/telemetry.c').read_text(encoding='utf-8')


def function(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include "arm_poll.h"
#include "direct_protocol.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ONLINE_PERIOD_MS 90000U
#define TELEMETRY_PERIOD_MS 180000U
#define ARM_FAIL_THRESHOLD 3
#define TOPIC_HEARTBEAT "heartbeat"
arm_link_stat_t g_arm_stat;
OnlyReadDataDef g_only_read;
ReadWiteDataDef g_read_write;
uint16_t g_ctrl_param[CTRL_PARAM_COUNT];
static uint64_t clock_ms;
static uint32_t s_now_ms,s_last_hb_ms,s_last_online_ms,s_last_info_ms,s_last_config_ms;
static uint32_t s_debug_deadline_ms,s_debug_interval_ms,s_arm_fail_count;
static bool s_hb_requested,s_config_requested,s_fault_reported,s_debug_active,s_quiet,fresh=true,locked,online=true;
static unsigned sent;
static direct_bms_summary_t sample;
static uint32_t now_ms(void){return (uint32_t)clock_ms;}
static int64_t esp_timer_get_time(void){return (int64_t)clock_ms*1000;}
bool arm_poll_take_param(void){assert(!locked);locked=true;return true;}
void arm_poll_give_param(void){assert(locked);locked=false;}
void arm_poll_request_info(void){}
static void telemetry_request_config(void){s_config_requested=true;}
static void publish_online(void){}
static void publish_identity_changes(void){}
static void publish_info(const OnlyReadDataDef*p){(void)p;assert(!locked);}
static void publish_config(const uint16_t*p,const bool*v,uint32_t r){(void)p;(void)v;(void)r;assert(!locked);}
static bool direct_config_snapshot_wire(uint16_t*p,bool*v,uint32_t*r){(void)p;(void)v;(void)r;assert(!locked);return false;}
static void publish_arm_recovery_alarm(void){}
static void publish_arm_fault_alarm(void){}
static void publish_ble_telemetry(const RunParamDef*p){(void)p;}
static void check_alarm(const RunParamDef*p){(void)p;}
static bool direct_dsp_get_measurement(RunParamDef*p,uint32_t*r){
 assert(!locked);memset(p,0xff,sizeof(*p));*r=now_ms();return fresh;
}
bool direct_bms_get_summary(direct_bms_summary_t*out){assert(!locked);*out=sample;return sample.acquired;}
static int mqtt_publish_heartbeat_raw(const char*t,const char*data,int len){
 assert(!locked && !strcmp(t,"heartbeat") && (size_t)len==strlen(data));
 if(!online)return -1;
 puts(data);sent++;return 0;
}
FUNCTIONS
static void report(void){g_arm_stat.last_rx_ms=now_ms();s_hb_requested=true;telemetry_tick();assert(!locked);}
int main(void){
 clock_ms=1000;report();
 sample.acquired=true;sample.received_ms=clock_ms;
 memset(sample.bytes,255,sizeof(sample.bytes));report();
 clock_ms+=120001;report();
 clock_ms=sample.received_ms+UINT32_MAX+5000ULL;report();
 sample.received_ms=clock_ms;memset(sample.bytes,0,sizeof(sample.bytes));report();
 unsigned before=sent;fresh=false;report();assert(sent==before);
 fresh=true;s_quiet=true;report();assert(sent==before);
 s_quiet=false;online=false;report();assert(sent==before);
 online=true;telemetry_on_mqtt_connected();telemetry_tick();assert(sent==before+1);
 clock_ms+=180000;g_arm_stat.last_rx_ms=now_ms();telemetry_tick();assert(sent==before+2);
 return 0;
}
'''
parts = [function(sig) for sig in (
    'static void hb_append(', 'static void publish_heartbeat(',
    'void telemetry_tick(', 'void telemetry_on_mqtt_connected(')]
target = ROOT / 'tmp/telemetry_bms_test.c'
target.parent.mkdir(exist_ok=True)
target.write_text(harness.replace('FUNCTIONS', '\n'.join(parts)), encoding='utf-8')
exe = target.with_suffix('.exe')
subprocess.run([str(GCC), '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-Imain/direct', '-Imain/telemetry', str(target), '-o', str(exe)], cwd=ROOT, check=True)
reports = [json.loads(line) for line in subprocess.check_output([str(exe)], text=True).splitlines()]
assert len(reports) == 7
assert 'bms_summary' not in reports[0]
for report in reports:
    assert len(json.dumps(report, separators=(',', ':'))) < 1024
    assert report['run'] == [65535] * len(report['run'])
for report, age in zip(reports[1:], (0, 120001, 4294967295, 0, 0, 180000)):
    summary = report.get('bms_summary')
    assert summary is not None, 'valid BMS snapshot missing from heartbeat'
    assert summary['layout'] == 0 and len(summary['bytes']) == 100
    assert summary['age_ms'] == age
assert reports[1]['bms_summary']['bytes'] == [255] * 100
assert reports[4]['bms_summary']['bytes'] == [0] * 100
print('PASS real heartbeat JSON: BMS missing/fresh/stale/wrap/zero, periodic/reconnect/query, DSP freshness, OTA quiet, publish outside mutex')
