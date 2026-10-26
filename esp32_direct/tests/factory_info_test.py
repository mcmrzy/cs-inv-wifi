"""Compile production identity JSON with the real cJSON implementation."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GCC = r'C:\Users\29538\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe'
CJSON = Path(r'D:\.espressif\v5.5.4\esp-idf\components\json\cJSON')

class FactoryInfoTest(unittest.TestCase):
    def test_identity_json_and_reboot_boundary(self):
        self.assertTrue((ROOT/'main/factory/factory_json.c').exists(), 'shared identity JSON formatter missing')
        code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "factory_json.h"
#include "mqtt_envelope.h"
#include <stdlib.h>
static void emit(factory_result_t status, factory_record_t *r, unsigned runtime){
    cJSON *j=factory_info_json("H1ZZX0013900001P","0.6.test","02:03:04:05:06:07",status,r,runtime,false);
    assert(j);char *s=cJSON_PrintUnformatted(j);assert(s);
    char *wrapped=NULL;size_t length=0;
    assert(mqtt_envelope_build(s,4294967295U,2,&wrapped,&length)==0);
    assert(length<MQTT_ENVELOPE_MAX);free(wrapped);
    puts(s);cJSON_free(s);cJSON_Delete(j);
}
int main(void){
    factory_record_t r={0};r.schema=2;r.generation=7;
    strcpy(r.sn,"H1ZZX0013900001P");strcpy(r.model,"CS-L10-6K2");strcpy(r.product,"CSERGY");
    strcpy(r.hw_system,"2.1");strcpy(r.hw_controller,"1.2");
    strcpy(r.controller_type,"CONTROL");strcpy(r.controller_code,"C3-001");strcpy(r.controller_sn,"CTRL0001");
    strcpy(r.work_order,"ORDER\"\\01");strcpy(r.station_id,"ST01");
    emit(FACTORY_UNPROVISIONED,NULL,0);emit(FACTORY_OK,&r,7);
    emit(FACTORY_OK,&r,6);strcpy(r.sn,"H1ZZX0013900002P");emit(FACTORY_OK,&r,7);
    /* Every field at maximum printable (JSON-escaped) capacity. */
    strcpy(r.sn,"H1ZZX0013900001P");
    char *base[]={r.product,r.batch,r.calibration_version,r.work_order,r.station_id};
    for(unsigned i=0;i<sizeof(base)/sizeof(base[0]);i++){memset(base[i],'"',31);base[i][31]=0;}
    memset(r.calibration_summary,'\\',63);r.calibration_summary[63]=0;
    char *codes[]={r.controller_code,r.controller_sn,r.dsp_code,r.dsp_sn,r.bms_code,r.bms_sn,r.power_code,r.power_sn};
    for(unsigned i=0;i<8;i++){memset(codes[i],'"',31);codes[i][31]=0;}
    emit(FACTORY_OK,&r,7);
    cJSON *inspect=cJSON_CreateObject();assert(factory_record_json(inspect,&r));
    char *s=cJSON_PrintUnformatted(inspect);assert(s);puts(s);cJSON_free(s);cJSON_Delete(inspect);
    return 0;
}
'''
        (ROOT/'tmp').mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=ROOT/'tmp') as folder:
            source=Path(folder)/'test.c';exe=Path(folder)/'test.exe'
            source.write_text(code,encoding='utf-8')
            subprocess.run([GCC,'-save-temps=obj','-std=c11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'main/factory'),'-I'+str(ROOT/'main/mqtt'),'-I'+str(CJSON),str(source),str(ROOT/'main/factory/factory_json.c'),str(ROOT/'main/factory/factory_record.c'),str(ROOT/'main/mqtt/mqtt_envelope.c'),str(CJSON/'cJSON.c'),'-lm','-o',str(exe)],check=True)
            rows=[json.loads(s) for s in subprocess.check_output([str(exe)],encoding='utf-8').splitlines()]
        empty,active,pending,changed,longest,inspect=rows
        self.assertEqual(empty['factory_identity']['status'],'UNPROVISIONED')
        self.assertIsNone(empty['hardware_version'])
        self.assertTrue(all(b['sn'] is None for b in empty['boards']))
        self.assertEqual(active['factory_identity']['generation'],7)
        self.assertFalse(active['factory_identity']['pending_reboot'])
        self.assertEqual(active['hardware_version'],'2.1')
        self.assertEqual(active['factory_identity']['work_order'],'ORDER"\\01')
        self.assertEqual(active['boards'][0]['code'],'C3-001')
        self.assertIsNone(active['boards'][1]['hardware_version'])
        for obj in (pending,changed):
            self.assertTrue(obj['factory_identity']['pending_reboot'])
            self.assertEqual(obj['sn'],'H1ZZX0013900001P')
            self.assertIsNone(obj['hardware_version'])
        for obj in rows[:5]:
            self.assertIsNone(obj['firmware_dsp']);self.assertIsNone(obj['firmware_bms'])
            self.assertNotIn('pin',json.dumps(obj).lower())
            self.assertLess(len(json.dumps(obj).encode()),4096)
        self.assertEqual(inspect['controller_sn'],'"'*31)
        self.assertEqual(inspect['schema'],2)

if __name__=='__main__':unittest.main()
