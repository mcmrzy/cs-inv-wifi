"""Run real HTTP OTA resume functions against controlled response headers."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

from ota_https_memory_test import GCC, ROOT, function


class HttpResumeTests(unittest.TestCase):
    def test_only_exact_single_response_range_is_accepted(self):
        source = (ROOT / 'main/ota/ota_service.c').read_text(encoding='utf-8')
        defines = '\n'.join(re.findall(r'^#define OTA_(?:BUFFER_SIZE|HTTP_MAX_REDIRECTS)\s+.*$', source, re.M))
        helpers = ''
        if 'static bool http_range_number(' in source:
            start = source.index('typedef struct {', source.index('static int http_stream_read('))
            end = source.index('static bool http_range_number(', start)
            helpers = source[start:end] + '\n'.join(function(source, name) for name in (
                'http_range_number', 'http_range_value', 'ota_http_event_handler'))
        code = r'''
#include "ota_service.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define ESP_OK 0
#define ESP_LOGW(...) ((void)0)
#define HTTP_EVENT_ON_HEADER 3
#define HTTP_TLS_DYN_BUF_RX_STATIC 1
typedef int esp_err_t;
typedef void *esp_http_client_handle_t;
typedef struct {int event_id;char *header_key;char *header_value;} esp_http_client_event_t;
typedef esp_err_t (*event_fn)(esp_http_client_event_t*);
typedef struct {const char *url;int timeout_ms;bool keep_alive_enable;int buffer_size;
 bool skip_cert_common_name_check;int max_redirection_count;int tls_dyn_buf_strategy;
 event_fn event_handler;} esp_http_client_config_t;
static OtaManifest g_manifest;
static event_fn handler;
static const char *first,*second,*key="Content-Range";
static int status=206;
static int64_t length=4;
static uint32_t offset=4;
static unsigned opens,closes;
static esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t*c){handler=c->event_handler;return (void*)1;}
static void esp_http_client_close(void*c){assert(c==(void*)1);closes++;}
static int esp_http_client_set_header(void*c,const char*k,const char*v){
 char expected[40];snprintf(expected,sizeof(expected),"bytes=%lu-",(unsigned long)offset);
 assert(c==(void*)1&&!strcmp(k,"Range")&&!strcmp(v,expected));return 0;
}
static int esp_http_client_open(void*c,int n){assert(c==(void*)1&&n==0);opens++;return 0;}
static int64_t esp_http_client_fetch_headers(void*c){
 assert(c==(void*)1);
 if(handler){
  esp_http_client_event_t e={.event_id=HTTP_EVENT_ON_HEADER,.header_key="Other",.header_value="ignored"};handler(&e);
  e.header_key=(char*)key;
  if(first){e.header_value=(char*)first;handler(&e);}
  if(second){e.header_value=(char*)second;handler(&e);}
 }
 return length;
}
static int esp_http_client_get_status_code(void*c){assert(c==(void*)1);return status;}
static void mqtt_publish_ota_trace(const char*a,const char*b){assert(a&&b);}
DEFINES
HELPERS
INIT
RESUME
static bool check(const char*a,const char*b){first=a;second=b;return http_stream_resume((void*)1,offset);}
int main(void){
 g_manifest=(OtaManifest){.file_size=8,.timeout_seconds=120};int size=0;
 assert(http_client_init_with_fallback(&g_manifest,&size));
 assert(!check(NULL,NULL));
 assert(check("bytes 4-7/8",NULL));
 assert(!check("bytes 4-7/8","bytes 4-7/8"));
 assert(!check("bytes 0-3/8",NULL));
 assert(!check("bytes 4-6/8",NULL));
 assert(!check("bytes 4-7/9",NULL));
 assert(!check("bytes 4-7/*",NULL));
 assert(!check("bytes */8",NULL));
 assert(!check("bytes +4-7/8",NULL));
 assert(!check("bytes 4-7/8junk",NULL));
 assert(!check("bytes 4294967300-7/8",NULL));
 assert(!check("bytes 4-4294967303/8",NULL));
 assert(!check("bytes 4-7/4294967304",NULL));
 assert(!check(NULL,NULL)); /* Prior valid response cannot leak into a reconnect. */
 key="cOnTeNt-RaNgE";assert(check("bytes 4-7/8",NULL));
 status=200;assert(!check("bytes 4-7/8",NULL));status=206;
 length=3;assert(!check("bytes 4-7/8",NULL));length=-1;assert(!check("bytes 4-7/8",NULL));
 length=4;offset=8;unsigned before=opens;assert(!check("bytes 8-7/8",NULL)&&opens==before);
 offset=9;assert(!check("bytes 9-7/8",NULL)&&opens==before);
 offset=0;length=8;assert(check("bytes 0-7/8",NULL));
 g_manifest.file_size=UINT32_MAX;offset=UINT32_MAX-1;length=1;
 assert(check("bytes 4294967294-4294967294/4294967295",NULL));
 assert(!check("bytes 4294967294-4294967294/4294967296",NULL));
 assert(closes>=opens);
 puts("PASS HTTP resume: exact response range required; missing, duplicate, overflow, stale, wrong status/length/offset/total/end rejected");
}
'''.replace('DEFINES', defines).replace('HELPERS', helpers).replace(
            'INIT', function(source, 'http_client_init_with_fallback')).replace(
            'RESUME', function(source, 'http_stream_resume'))
        with tempfile.TemporaryDirectory(dir=ROOT / 'tmp', prefix='ota_range_') as folder:
            c, exe = Path(folder) / 'test.c', Path(folder) / 'test.exe'
            c.write_text(code, encoding='utf-8')
            env = os.environ.copy()
            env['PATH'] = str(GCC.parent) + os.pathsep + env.get('PATH', '')
            env['TMP'] = env['TEMP'] = folder
            subprocess.run([str(GCC), '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(ROOT / 'main/ota'), str(c), '-o', str(exe)],
                           env=env, check=True, timeout=30)
            result = subprocess.run([str(exe)], env=env, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, (result.stdout + result.stderr).decode(errors='replace'))
            print(result.stdout.decode().strip())


if __name__ == '__main__':
    unittest.main()
