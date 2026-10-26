"""Compile the real info formatter; validate JSON without invented DSP identity."""
from pathlib import Path
import json
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GCC = Path(r'C:\Users\29538\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe')
SOURCE = (ROOT / 'main/telemetry/telemetry.c').read_text(encoding='utf-8')

def function(signature):
    start = SOURCE.index(signature)
    cursor = SOURCE.index('{', start) + 1
    depth = 1
    while depth:
        depth += (SOURCE[cursor] == '{') - (SOURCE[cursor] == '}')
        cursor += 1
    return SOURCE[start:cursor]

class DspInfoTest(unittest.TestCase):
    def test_actual_formatter_never_fabricates_missing_dsp_identity(self):
        shim = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "arm_param.h"
#include "factory_json.h"
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define TOPIC_INFO "info"
#define FIRMWARE_VERSION "0.6.7"
static char g_device_sn[33]="H1ZZX00139000081";
static bool connected;
static unsigned published;
static unsigned clock_ms, revision;
static int queue_result=1;
static atomic_uint s_info_revision_sent,s_info_revision_attempted,s_info_last_attempt;
static atomic_bool s_info_pending;
static unsigned now_ms(void){return clock_ms;}
static unsigned factory_identity_revision(void){return revision;}
static unsigned factory_identity_runtime_generation(void){return 0;}
static factory_result_t factory_identity_read(factory_record_t *r){(void)r;return FACTORY_UNPROVISIONED;}
static bool factory_identity_admin_sn(char *s,size_t n){(void)s;(void)n;return false;}
#define ESP_MAC_WIFI_STA 0
static int esp_read_mac(unsigned char *out,int kind){(void)kind;memset(out,2,6);return 0;}
static bool mqtt_is_connected(void){return connected;}
static int mqtt_publish_volatile(const char *topic,const char *data,int length,int qos){
    assert(!strcmp(topic,"info") && length==0 && qos==1);
    published++; puts(data); return queue_result;
}
'''
        code = shim + function('static void publish_info(') + function('static void publish_identity_changes(') + r'''
int main(void){
    OnlyReadDataDef legacy;memset(&legacy,0xff,sizeof(legacy));
    publish_info(&legacy);assert(published==0);
    connected=true;publish_info(&legacy);assert(published==1);
    publish_identity_changes();assert(published==1);
    revision=1;queue_result=-1;publish_identity_changes();assert(published==2 && s_info_pending);
    clock_ms=4999;publish_identity_changes();assert(published==2);
    clock_ms=5000;queue_result=1;publish_identity_changes();assert(published==3 && !s_info_pending);
    clock_ms=20000;publish_identity_changes();assert(published==3);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(dir=ROOT / 'tmp') as folder:
            source = Path(folder) / 'info.c'
            exe = Path(folder) / 'info.exe'
            source.write_text(code, encoding='utf-8')
            cjson=Path(r'D:\.espressif\v5.5.4\esp-idf\components\json\cJSON')
            subprocess.run([str(GCC), '-save-temps=obj','-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-I'+str(ROOT / 'main/telemetry'),'-I'+str(ROOT/'main/factory'),'-I'+str(cjson),str(source),
                            str(ROOT/'main/factory/factory_json.c'),str(ROOT/'main/factory/factory_record.c'),str(cjson/'cJSON.c'),'-lm','-o', str(exe)], check=True)
            output = subprocess.check_output([str(exe)], encoding='utf-8')
        rows=[json.loads(line) for line in output.splitlines()]
        self.assertEqual(len(rows),3)
        info = rows[0]
        self.assertEqual(info['sn'], 'H1ZZX00139000081')
        self.assertEqual(info['firmware_esp'], '0.6.7')
        self.assertEqual(info['model'], 'CS-L10-6K2')
        for field in ('firmware_arm','firmware_dsp','firmware_bms','rated_power',
                      'rated_voltage','rated_frequency','battery_nominal_voltage',
                      'battery_type','cell_count','temp_sensor_count',
                      'inverter_module','hardware_version','bootloader_version'):
            self.assertIsNone(info[field], field)
        self.assertFalse(info['dsp_identity_available'])
        self.assertEqual(info['factory_identity']['status'],'UNPROVISIONED')
        self.assertLess(len(output.encode('utf-8')),4096*3)

if __name__ == '__main__':
    unittest.main()
