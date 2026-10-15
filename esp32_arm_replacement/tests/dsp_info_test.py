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
#include "arm_param.h"
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define TOPIC_INFO "info"
#define FIRMWARE_VERSION "0.6.7"
static char g_device_sn[33]="H1ZZX00139000081";
static bool connected;
static unsigned published;
static bool mqtt_is_connected(void){return connected;}
static int mqtt_publish(const char *topic,const char *data,int length,int qos,int retain){
    assert(!strcmp(topic,"info") && length==0 && qos==1 && retain==0);
    published++; puts(data); return 1;
}
'''
        code = shim + function('static void publish_info(') + r'''
int main(void){
    OnlyReadDataDef legacy;memset(&legacy,0xff,sizeof(legacy));
    publish_info(&legacy);assert(published==0);
    connected=true;publish_info(&legacy);assert(published==1);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(dir=ROOT / 'tmp') as folder:
            source = Path(folder) / 'info.c'
            exe = Path(folder) / 'info.exe'
            source.write_text(code, encoding='utf-8')
            subprocess.run([str(GCC), '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-I'+str(ROOT / 'main/telemetry'), str(source), '-o', str(exe)], check=True)
            output = subprocess.check_output([str(exe)], encoding='utf-8')
        info = json.loads(output)
        self.assertEqual(info['sn'], 'H1ZZX00139000081')
        self.assertEqual(info['firmware_esp'], '0.6.7')
        self.assertEqual(info['model'], 'CS-L10-6K2')
        for field in ('firmware_arm','firmware_dsp','firmware_bms','rated_power',
                      'rated_voltage','rated_frequency','battery_nominal_voltage',
                      'battery_type','cell_count','temp_sensor_count',
                      'inverter_module','hardware_version','bootloader_version'):
            self.assertIsNone(info[field], field)
        self.assertFalse(info['dsp_identity_available'])
        self.assertLess(len(output.encode('utf-8')), 640)

if __name__ == '__main__':
    unittest.main()
