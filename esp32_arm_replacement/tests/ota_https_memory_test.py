"""Execute OTA HTTP setup and stream recovery with fake IDF boundaries.

Hardware must still confirm TLS allocation, download, reboot and version readback.
The host checks exercise the real firmware functions, including signed read errors.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GCC = Path(r'C:\Users\29538\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe')


def function(source, name):
    match = re.search(r'^static [^\n]+\b' + name + r'\(', source, re.M)
    assert match, name
    end = source.index('{', match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


class HttpsMemoryTests(unittest.TestCase):
    def run_c(self, code, extra=()):
        with tempfile.TemporaryDirectory(dir=ROOT / 'tmp', prefix='ota_https_') as folder:
            c = Path(folder) / 'test.c'
            exe = Path(folder) / 'test.exe'
            c.write_text(code, encoding='utf-8')
            env = os.environ.copy()
            env['PATH'] = str(GCC.parent) + os.pathsep + env.get('PATH', '')
            env['TMP'] = env['TEMP'] = folder
            subprocess.run([str(GCC), '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(ROOT / 'main/ota'), str(c), *extra, '-o', str(exe)],
                           env=env, check=True, timeout=30)
            result = subprocess.run([str(exe)], env=env, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, (result.stdout + result.stderr).decode(errors='replace'))
            print(result.stdout.decode().strip())

    def test_http_reserves_tls_receive_buffer_and_bounds_auxiliary_memory(self):
        source = (ROOT / 'main/ota/ota_service.c').read_text(encoding='utf-8')
        defines = '\n'.join(re.findall(r'^#define OTA_(?:BUFFER_SIZE|HTTP_MAX_REDIRECTS)\s+.*$', source, re.M))
        code = r'''
#include "ota_transfer_policy.h"
#include <assert.h>
#include <stdio.h>
#define HTTP_TLS_DYN_BUF_RX_STATIC 1
#define MALLOC_CAP_8BIT 1
#define ESP_LOGW(...) ((void)0)
typedef struct {const char *url;int timeout_ms;bool keep_alive_enable;int buffer_size;
 bool skip_cert_common_name_check;int max_redirection_count;int tls_dyn_buf_strategy;
 int (*event_handler)(void*);} esp_http_client_config_t;
typedef void *esp_http_client_handle_t;
static int ota_http_event_handler(void*event){(void)event;return 0;}
static unsigned calls, reject;
static esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c){
 assert(c->buffer_size<=1024);assert(c->tls_dyn_buf_strategy==HTTP_TLS_DYN_BUF_RX_STATIC);
 assert(c->event_handler==ota_http_event_handler);
 assert(c->skip_cert_common_name_check && c->keep_alive_enable && c->timeout_ms==120000);
 calls++;return calls<=reject?NULL:(void*)1;
}
DEFINES
FUNCTION
int main(void){
 OtaManifest m={.target=OTA_TARGET_ESP,.source=OTA_SOURCE_MQTT,.timeout_seconds=120};
 int size=0;assert(http_client_init_with_fallback(&m,&size) && size==1024 && calls==1);
 calls=0;reject=1;assert(http_client_init_with_fallback(&m,&size) && size==512 && calls==2);
 calls=0;reject=100;assert(!http_client_init_with_fallback(&m,&size));
 for(unsigned target=OTA_TARGET_ESP;target<=OTA_TARGET_BMS;target++){
  m.target=(OtaTarget)target;m.source=OTA_SOURCE_MQTT;
  OtaTransferPolicy p=ota_transfer_policy(&m);assert(p.buffer_size<=1024);
  assert(p.sequential_erase==(target==OTA_TARGET_ESP));
 }
 m.target=OTA_TARGET_ESP;m.source=OTA_SOURCE_LOCAL;
 OtaTransferPolicy p=ota_transfer_policy(&m);assert(p.buffer_size==496&&p.sequential_erase);
 puts("PASS HTTPS: bounded HTTP/transfer buffers, static full TLS RX strategy, allocation fallback, BLE policy preserved");
}
'''.replace('DEFINES', defines).replace('FUNCTION', function(source, 'http_client_init_with_fallback'))
        self.run_c(code, [str(ROOT / 'main/ota/ota_transfer_policy.c')])

    def test_negative_reader_error_resumes_from_committed_offset(self):
        source = (ROOT / 'main/ota/ota_service.c').read_text(encoding='utf-8')
        code = r'''
#include "ota_service.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define OTA_READ_IDLE_MS 10
#define pdMS_TO_TICKS(x) (x)
#define ESP_OK 0
#define ESP_LOGW(...) ((void)0)
typedef unsigned esp_ota_handle_t;
typedef void *esp_http_client_handle_t;
typedef bool (*OtaResumeFn)(void*,uint32_t);
typedef struct {unsigned bytes;} mbedtls_sha256_context;
static unsigned read_calls,resumes,written,hash_bytes,notices,file_bytes;
static uint32_t clock_ms;
static int failure_kind;
static bool do_cancel,resume_ok;
static FILE *g_direct_stage;
static bool cancel_requested(void){return do_cancel;}
static int64_t esp_timer_get_time(void){return (int64_t)clock_ms*1000;}
static void vTaskDelay(unsigned ms){clock_ms+=ms;}
static bool esp_http_client_is_complete_data_received(void*c){(void)c;return false;}
static void mqtt_publish_ota_trace(const char*a,const char*b){assert(a&&b);notices++;}
static void mbedtls_sha256_init(mbedtls_sha256_context*c){c->bytes=0;}
static int mbedtls_sha256_starts(mbedtls_sha256_context*c,int n){(void)c;(void)n;return 0;}
static int mbedtls_sha256_update(mbedtls_sha256_context*c,const uint8_t*b,size_t n){assert(b);c->bytes+=(unsigned)n;hash_bytes+=(unsigned)n;return 0;}
static void mbedtls_sha256_free(mbedtls_sha256_context*c){(void)c;}
static int mbedtls_sha256_finish(mbedtls_sha256_context*c,uint8_t*d){assert(c->bytes==file_bytes);memset(d,0,32);return 0;}
static void hash_to_hex(const uint8_t*b,char*h){assert(b);memset(h,'0',64);h[64]=0;}
static int esp_ota_write(esp_ota_handle_t h,const uint8_t*b,size_t n){assert(h==1&&b);written+=(unsigned)n;return 0;}
static void update_snapshot_stage(OtaState s,uint8_t p,uint32_t n,OtaResult r,const char*m,const char*stage,uint8_t sp){(void)s;assert(p<=100&&n<=file_bytes&&r==OTA_RESULT_OK&&m&&stage&&sp==p);}
FUNCTION
static int reader(void*c,uint8_t*b,size_t n){
 assert(c==(void*)1);read_calls++;
 if(read_calls==2 || (failure_kind>=3 && read_calls%2==0)){if(failure_kind==1)return (int)n+1;if(failure_kind==2){do_cancel=true;return -1;}return -1;}
 assert(n>=4);memset(b,7,4);return 4;
}
static bool resume_at(void*c,uint32_t offset){assert(c==(void*)1&&offset==written&&offset==hash_bytes&&offset%4==0);resumes++;return resume_ok;}
static OtaResult run(unsigned attempts,int kind,bool ok){
 read_calls=resumes=written=hash_bytes=notices=clock_ms=0;do_cancel=false;failure_kind=kind;resume_ok=ok;
 file_bytes=kind==3?16:kind==4?20:8;
 OtaManifest m={.target=OTA_TARGET_ESP,.source=OTA_SOURCE_MQTT,.file_size=file_bytes,.timeout_seconds=120};
 memset(m.sha256,'0',64);m.sha256[64]=0;uint8_t b[4];esp_ota_handle_t h=1;
 return read_and_stage_buffered(&m,reader,(void*)1,&h,resume_at,attempts,b,sizeof(b));
}
int main(void){
 assert(run(3,0,true)==OTA_RESULT_OK&&resumes==1&&written==8&&hash_bytes==8);
 assert(run(0,0,true)==OTA_RESULT_READ_FAILED&&resumes==0&&written==4);
 assert(run(3,0,false)==OTA_RESULT_READ_FAILED&&resumes==1&&written==4);
 assert(run(3,1,true)==OTA_RESULT_READ_FAILED&&resumes==0&&written==4);
 assert(run(3,2,true)==OTA_RESULT_READ_FAILED&&resumes==0&&written==4);
 assert(run(3,3,true)==OTA_RESULT_OK&&resumes==3&&written==16&&hash_bytes==16);
 assert(run(3,4,true)==OTA_RESULT_READ_FAILED&&resumes==3&&written==16&&read_calls==8);
 puts("PASS stream: signed -1 triggers bounded recovery at committed offset; oversize, cancel, exhausted and refused resume stay safe");
}
'''.replace('FUNCTION', function(source, 'read_and_stage_buffered'))
        self.run_c(code)


if __name__ == '__main__':
    unittest.main()
