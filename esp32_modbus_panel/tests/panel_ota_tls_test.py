"""Execute production panel remote-download admission, TLS config and redirects."""
from pathlib import Path
import re
import subprocess
import sys
root=Path(__file__).resolve().parents[1]
source=(root/'full/ota_service.c').read_text(encoding='utf-8')
def function(name):
 m=re.search(r'^(?:static\s+)?(?:OtaResult|esp_http_client_handle_t)\s+'+name+r'\([^;]*?\)\s*\{',source,re.M)
 assert m,name
 end=source.index('{',m.start())+1;depth=1
 while depth:depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[m.start():end]
test=r'''
#include "ota_service.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define ESP_OK 0
#define pdTRUE 1
#define portMAX_DELAY 999
#define OTA_BUFFER_SIZE 1024
#define OTA_HTTP_MAX_REDIRECTS 3
#define HTTP_TLS_DYN_BUF_RX_STATIC 1
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
typedef int esp_err_t;
typedef void *esp_http_client_handle_t;
typedef struct {
 const char *url;int timeout_ms;bool keep_alive_enable;int(*event_handler)(void *);
 int buffer_size,tls_dyn_buf_strategy;int(*crt_bundle_attach)(void *);
 bool skip_cert_common_name_check;int max_redirection_count;
} esp_http_client_config_t;
static esp_http_client_config_t actual;
static OtaManifest g_manifest;
static void *g_lock=(void *)1,*g_remote_coordinator=(void *)2;
static bool g_remote_cleanup_busy,g_remote_reboot_pending;
static unsigned authenticated,reserved,notified,opens,closed,response_index;
static bool redirect_http,url_error;
static int statuses[]={302,200};
static char current_url[256]="https://firmware.example/panel.bin";
static const char *esp_err_to_name(int e){(void)e;return "mock";}
static int ota_http_event_handler(void *e){(void)e;return 0;}
static int esp_crt_bundle_attach(void *e){(void)e;return 0;}
static unsigned esp_get_free_heap_size(void){return 100000;}
static esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c){actual=*c;return (void *)3;}
static int esp_http_client_open(esp_http_client_handle_t c,int n){assert(c==(void *)3&&!n);opens++;return 0;}
static int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c){(void)c;return 1000;}
static int esp_http_client_get_status_code(esp_http_client_handle_t c){(void)c;return statuses[response_index];}
static bool esp_http_client_is_chunked_response(esp_http_client_handle_t c){(void)c;return false;}
static int esp_http_client_set_redirection(esp_http_client_handle_t c){(void)c;strcpy(current_url,redirect_http?"http://firmware.example/plain.bin":"https://cdn.example/panel.bin");return 0;}
static int esp_http_client_get_url(esp_http_client_handle_t c,char *out,int size){(void)c;if(url_error)return 1;snprintf(out,(size_t)size,"%s",current_url);return 0;}
static void esp_http_client_close(esp_http_client_handle_t c){(void)c;closed++;response_index++;}
static OtaResult validate_manifest_cheap(const OtaManifest *m){return m?OTA_RESULT_OK:OTA_RESULT_INVALID_PARAMS;}
static OtaResult check_available(const OtaManifest *m){(void)m;return OTA_RESULT_OK;}
static OtaResult authenticate_manifest(const OtaManifest *m){(void)m;authenticated++;return OTA_RESULT_OK;}
static OtaResult reserve_manifest(const OtaManifest *m){g_manifest=*m;reserved++;return OTA_RESULT_OK;}
static void arm_ota_workspace_release(void){}
static int xSemaphoreTake(void *m,unsigned timeout){assert(m==g_lock&&timeout==portMAX_DELAY);return 1;}
static void xSemaphoreGive(void *m){assert(m==g_lock);}
static void finish_failure(OtaResult r,const char *s){(void)r;(void)s;assert(false);}
static void xTaskNotifyGive(void *t){assert(t==g_remote_coordinator);notified++;}
FUNCTIONS
int main(void){
 OtaManifest local={.source=OTA_SOURCE_LOCAL,.target=OTA_TARGET_ESP,.timeout_seconds=300};
 strcpy(local.url,"http://firmware.example/panel.bin");
 assert(ota_service_start_remote(&local)==OTA_RESULT_TLS_REQUIRED&&!authenticated&&!reserved&&!notified);
 strcpy(local.url,"https://firmware.example/panel.bin");
 assert(ota_service_start_remote(&local)==OTA_RESULT_OK&&authenticated==1&&reserved==1&&notified==1&&g_remote_cleanup_busy&&!g_remote_reboot_pending);
 int size;assert(http_client_init_with_fallback(&local,&size)==(void *)3&&size==1024);
 assert(actual.crt_bundle_attach==esp_crt_bundle_attach&&!actual.skip_cert_common_name_check);
 char detail[100];int status;int64_t length;
 redirect_http=true;assert(prepare_http_download((void *)3,1000,&status,&length,detail,sizeof(detail))==OTA_RESULT_TLS_REQUIRED&&opens==1&&!closed);
 opens=response_index=closed=0;redirect_http=false;
 assert(prepare_http_download((void *)3,1000,&status,&length,detail,sizeof(detail))==OTA_RESULT_OK&&opens==2&&closed==1);
 opens=response_index=closed=0;url_error=true;
 assert(prepare_http_download((void *)3,1000,&status,&length,detail,sizeof(detail))==OTA_RESULT_TLS_REQUIRED&&opens==1);
 url_error=false;OtaManifest cloud=local;cloud.source=OTA_SOURCE_MQTT;
 assert(http_client_init_with_fallback(&cloud,&size)==(void *)3&&!actual.crt_bundle_attach&&actual.skip_cert_common_name_check);
 assert(ota_service_start_remote(NULL)==OTA_RESULT_INVALID_PARAMS);
 puts("PASS panel OTA TLS: production HTTPS admission, CA bundle and hostname verification, HTTPS redirects, no downgrade or unreadable redirect, existing cloud config retained");
}
'''.replace('\nFUNCTIONS\n','\n'+'\n'.join(function(n) for n in ['prepare_http_download','http_client_init_with_fallback','ota_service_start_remote'])+'\n')
path=root/'tmp/panel_ota_tls_test.c';path.write_text(test,encoding='utf-8');exe=path.with_suffix('.exe')
subprocess.run([sys.argv[1],'-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-I'+str(root.parent/'esp32_direct/main/ota'),str(path),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
