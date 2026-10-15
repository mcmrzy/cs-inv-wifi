"""Execute real startup ordering and MQTT start against a fragmented heap model."""
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parents[1]
gcc = Path(r'C:\Users\29538\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe')
source = (root / 'main/mqtt/mqtt_handler.c').read_text(encoding='utf-8')
app = (root / 'main/app/app_main.c').read_text(encoding='utf-8')

def function(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

startup = app[app.index('    telemetry_init();', app.index('void app_main(')):]
startup = startup[:startup.index('\n}')]
defines = '\n'.join(re.findall(r'^#define MQTT_START_RETRY_MS\s+.*$', source, re.M))
declarations = '\n'.join(re.findall(r'^static int64_t g_next_start_ms[^;]*;', source, re.M))
dns_condition = re.search(r'const bool cannot_resolve\s*=.*?;', source, re.S).group()
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
DEFINES
DECLARATIONS
static void *g_mqtt_client=(void*)1;
static atomic_bool g_mqtt_connected,g_mqtt_started;
static int64_t g_disconnect_start_ms, clock_ms=1000;
static const char *TAG="MQTT",*g_mqtt_uri="mqtts://test";
static unsigned largest=16000, starts, memory_logs;
static bool wifi_initialized, ble_started;
static bool have_ip;
bool wifi_sta_is_connected(void){return have_ip;}
#define MQTT_ERROR_TYPE_TCP_TRANSPORT 1
#define ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME 2
static bool dns_failure(int kind,int code){
 struct{int error_type,esp_tls_last_esp_err;} value={kind,code},*err=&value;
 DNS_CONDITION
 return cannot_resolve;
}
static int64_t esp_timer_get_time(void){return clock_ms*1000;}
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
static const char*esp_err_to_name(int e){(void)e;return "ESP_FAIL";}
static void log_message(const char*t,const char*f,...){(void)t;(void)f;}
#define ESP_LOGI log_message
#define ESP_LOGE log_message
#define ESP_LOGW log_message
void mqtt_log_tls_memory(const char*p){assert(p);memory_logs++;}
static int esp_mqtt_client_start(void*c){assert(c);starts++;if(largest<8192)return ESP_FAIL;largest-=8192;return ESP_OK;}
CONNECT
#define NOOP(name) static void name(void){}
NOOP(telemetry_init) NOOP(cmd_handler_init) NOOP(mqtt_ota_init)
NOOP(offline_cache_init) NOOP(sntp_sync_init)
static bool mqtt_init(void*p){(void)p;return true;}
static bool ota_service_init(void){return true;}
static void wifi_sta_init(void){wifi_initialized=true;}
static void setup_wifi(void){assert(wifi_initialized);}
static bool ble_ct_start(const char*a,const char*b){assert(a&&b&&wifi_initialized);ble_started=true;largest=6000;return true;}
static bool ble_ota_init(void){return true;}
static bool ble_prov_start(const char*a,const char*b){(void)a;(void)b;return true;}
static const char*ble_prov_get_service_name(void){return "test";}
#define ble_ct_cmd_set_executors(a,b) ((void)0)
#define ble_prov_set_callback(a) ((void)0)
#define ble_prov_set_gap_event_forward(a) ((void)0)
#define ble_prov_set_pre_host_start_cb(a) ((void)0)
static void *cmd_handler_on_mqtt_message;
static char g_device_sn[]="test";
#define FIRMWARE_VERSION "test"
#define STATE_INIT 0
#define BOOT_FAILED 1
#define BOOT_READY 2
#define pdPASS 1
static atomic_int s_boot_stage;
static void *main_loop_task;
static void enter_state(int s){(void)s;}
static int xTaskCreate(void*f,const char*n,int s,void*p,int pr,void*h){(void)f;(void)n;(void)s;(void)p;(void)pr;(void)h;return pdPASS;}
static bool ota_service_start_boot_health_check(void){return true;}
static void startup(void){STARTUP}
int main(void){
 startup();
 mqtt_connect(); /* First main-loop poll after WiFi gets its IP. */
 assert(ble_started && starts==1 && atomic_load(&g_mqtt_started));
 /* Later reconnect polling must not create a second task. */
 assert(mqtt_connect() && starts==1);
 atomic_store(&g_mqtt_started,false);g_disconnect_start_ms=0;
 starts=0;clock_ms=10000;largest=6000;
 assert(!mqtt_connect() && starts==1 && g_disconnect_start_ms==clock_ms);
 for(int i=0;i<100;i++){clock_ms=10000+i*40;assert(!mqtt_connect());}
 assert(starts==1); /* Two polls/100 ms must not become two allocations. */
 clock_ms=15000;largest=14000;
 assert(mqtt_connect() && starts==2 && atomic_load(&g_mqtt_started));
 assert(memory_logs>=1);
 atomic_store(&g_mqtt_connected,true);assert(mqtt_connect() && starts==2);
 have_ip=false;assert(!dns_failure(1,2));
 have_ip=true;assert(dns_failure(1,2));assert(!dns_failure(0,2));assert(!dns_failure(1,0));
 puts("PASS real startup reserves 8KB MQTT task before BLE; failed starts back off, start timeout tracked, retry recovers, connected task not duplicated");
}
'''
for marker, value in [('DEFINES',defines),('DECLARATIONS',declarations),
                      ('DNS_CONDITION',dns_condition),
                      ('CONNECT',function(source,'bool mqtt_connect(')),('STARTUP',startup)]:
    harness = harness.replace(marker,value)
target=root/'tmp/mqtt_start_memory_test.c'
target.write_text(harness,encoding='utf-8')
subprocess.run([str(gcc),'-std=c11','-Wall','-Wextra','-Werror',str(target),'-o',str(target.with_suffix('.exe'))],check=True,timeout=30)
subprocess.run([str(target.with_suffix('.exe'))],check=True,timeout=10)
