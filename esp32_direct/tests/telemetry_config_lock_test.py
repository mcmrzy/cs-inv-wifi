"""Exercise actual MQTT reconnect -> tick -> settings -> mutex code together."""
from pathlib import Path
import json
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
GCC = Path(r'C:\Users\29538\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe')
tele = (ROOT / 'main/telemetry/telemetry.c').read_text(encoding='utf-8')
poll = (ROOT / 'main/direct/direct_poll.c').read_text(encoding='utf-8')


def function(source, name):
    match = re.search(r'^(?:static )?(?:void|bool|uint16_t) ' + name + r'\([^;]*?\)\s*\{', source, re.M)
    assert match, name
    brace = source.index('{', match.start())
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


start = tele.index('static const cfg_key_t s_cfg_keys')
table = tele[start:tele.index('};', start) + 2]
poll_functions = ['collector_value', 'arm_poll_take_param', 'arm_poll_give_param',
                  'direct_config_get', 'direct_config_get_wire']
if 'bool direct_config_snapshot_wire(' in poll:
    poll_functions.append('direct_config_snapshot_wire')
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "arm_poll.h"
#include "direct_dsp_params.h"
#include "direct_protocol.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
#define ONLINE_PERIOD_MS 90000U
#define TELEMETRY_PERIOD_MS 180000U
#define ARM_FAIL_THRESHOLD 3
#define TOPIC_CONFIG "config"
arm_link_stat_t g_arm_stat;
OnlyReadDataDef g_only_read;
ReadWiteDataDef g_read_write;
uint16_t g_ctrl_param[CTRL_PARAM_COUNT];
static int s_lock=1;
static direct_dsp_params_t s_dsp_params;
static bool s_dsp_params_valid, s_config_valid[DIRECT_DSP_CONFIG_COUNT];
static uint16_t s_config[DIRECT_DSP_CONFIG_COUNT];
static bool locked, s_hb_requested, s_config_requested, s_fault_reported, s_debug_active, s_quiet;
static unsigned take_count, fail_take_at, config_sent, info_sent;
static uint32_t clock_ms=1000, s_now_ms, s_last_hb_ms, s_last_online_ms, s_last_info_ms, s_last_config_ms;
static uint32_t s_debug_deadline_ms, s_debug_interval_ms, s_arm_fail_count;
static uint32_t now_ms(void){return clock_ms;}
static int xSemaphoreTake(int lock, unsigned timeout){
    assert(lock==1 && timeout==100);
    /* Models a nonrecursive FreeRTOS mutex. This reproduces v0.6.14. */
    assert(!locked && "recursive parameter mutex acquisition");
    if(++take_count==fail_take_at)return 0;
    locked=true;return pdTRUE;
}
static void xSemaphoreGive(int lock){assert(lock==1 && locked);locked=false;}
static void publish_online(void){assert(!locked);}
static void publish_identity_changes(void){assert(!locked);}
static void publish_info(const OnlyReadDataDef *p){(void)p;assert(!locked);info_sent++;}
void arm_poll_request_info(void){}
void arm_poll_request_config(void){}
static void publish_arm_recovery_alarm(void){assert(!locked);}
static void publish_arm_fault_alarm(void){assert(!locked);}
static void publish_heartbeat(const RunParamDef *p,const direct_bms_summary_t *b){(void)p;(void)b;assert(!locked);}
static bool direct_bms_get_summary(direct_bms_summary_t *b){(void)b;assert(!locked);return false;}
static void publish_ble_telemetry(const RunParamDef *p){(void)p;assert(!locked);}
static void check_alarm(const RunParamDef *p){(void)p;assert(!locked);}
static bool direct_dsp_get_measurement(RunParamDef *p,uint32_t *ms){(void)p;(void)ms;assert(!locked);return false;}
static int mqtt_publish(const char *topic,const char *data,int length,int qos,int retain){
    assert(!locked && !strcmp(topic,"config") && length==0 && qos==1 && retain==0);
    config_sent++;puts(data);return 1;
}
typedef struct {const char *name;uint8_t scale;} cfg_key_t;
TABLE
POLL_FUNCTIONS
PUBLISH
REQUEST_CONFIG
ON_CONNECTED
TICK
int main(void){
    assert(direct_dsp_params_arm_defaults(&s_dsp_params));s_dsp_params_valid=true;
    s_dsp_params.revision=7;g_read_write.CtrlParamAlterTime=7;
    g_arm_stat.last_ctrl_ms=clock_ms;g_arm_stat.last_rx_ms=clock_ms;
    telemetry_on_mqtt_connected();telemetry_tick();assert(config_sent==1 && !locked && !s_config_requested);
    telemetry_tick();assert(config_sent==1);
    /* MQTT reconnect must report even with the same saved revision. */
    telemetry_on_mqtt_connected();telemetry_tick();assert(config_sent==2);
    assert(direct_dsp_params_set(&s_dsp_params,7,2400));
    assert(direct_dsp_params_set(&s_dsp_params,5,0));
    g_read_write.CtrlParamAlterTime=8;g_arm_stat.last_ctrl_ms=++clock_ms;
    telemetry_request_config();
    /* Another task temporarily owns the snapshot mutex: retain the request. */
    fail_take_at=take_count+2;telemetry_tick();assert(config_sent==2 && s_config_requested && !locked);
    fail_take_at=0;telemetry_tick();assert(config_sent==3 && !s_config_requested && !locked);
    /* Info and config in the same tick must both publish outside the lock. */
    g_arm_stat.last_info_ms=++clock_ms;telemetry_request_config();telemetry_tick();
    assert(config_sent==4 && info_sent==3 && !locked);
    /* First metadata acquisition can also time out without losing the request. */
    telemetry_request_config();fail_take_at=take_count+1;telemetry_tick();assert(config_sent==4 && s_config_requested);
    fail_take_at=0;telemetry_tick();assert(config_sent==5 && !s_config_requested && !locked);
    /* Saved controller values take priority over stale DSP observations. */
    s_config[7]=2100;s_config_valid[7]=true;
    g_read_write.CtrlParamAlterTime=9;g_arm_stat.last_ctrl_ms=++clock_ms;
    telemetry_request_config();telemetry_tick();assert(config_sent==6 && !locked);
}
'''
for marker, content in {
    'TABLE': table,
    'POLL_FUNCTIONS': '\n'.join(function(poll, name) for name in poll_functions),
    'PUBLISH': function(tele, 'publish_config'),
    'REQUEST_CONFIG': function(tele, 'telemetry_request_config'),
    'ON_CONNECTED': function(tele, 'telemetry_on_mqtt_connected'),
    'TICK': function(tele, 'telemetry_tick'),
}.items():
    code = code.replace('\n' + marker + '\n', '\n' + content + '\n')
target = ROOT / 'tmp/telemetry_config_lock_test.c'
target.write_text(code, encoding='utf-8')
exe = target.with_suffix('.exe')
subprocess.run([str(GCC), '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-Imain/direct', '-Imain/telemetry', str(target),
                'main/direct/direct_protocol.c', 'main/direct/direct_dsp_params.c', 'main/direct/direct_battery_config.c', '-o', str(exe)],
               cwd=ROOT, check=True)
reports = [json.loads(line) for line in subprocess.check_output([str(exe)], text=True).splitlines()]
assert [report['rev'] for report in reports] == [7, 7, 8, 8, 8, 9]
for i, report in enumerate(reports):
    params = report['params']
    assert params['set_output_voltage'] == (230 if i < 2 else 240)
    assert params['set_output_frequency'] == 50
    assert params['set_max_charge_current'] == 140 and params['set_ac_charge_current'] == 60
    assert params['set_overload_restart'] == (1 if i < 2 else 0)
    assert 'set_battery_capacity' not in params and 'set_max_discharge_current' not in params
print('PASS actual telemetry config mutex integration: connect/reconnect/save/query, no recursive lock, publish outside lock, coherent revision, contention retries, known zero and unknown omission')
