"""Execute real app mode decisions. Fake only hardware/SDK boundaries."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest
from mqtt_connection_window_test import function, GCC

ROOT = Path(__file__).resolve().parents[1]


class LocalModeTests(unittest.TestCase):
    def test_existing_cache_worker_stops_before_the_next_cached_item(self):
        mqtt=(ROOT/'main/mqtt/mqtt_handler.c').read_text(encoding='utf-8')
        harness=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <stdio.h>
#include <inttypes.h>
#define ESP_LOGI(...) ((void)0)
#define PROTO_TOPIC_FMT "cs_inv/%s/%s"
#define PROTO_ENVELOPE_VERSION 1
#define OFFLINE_ACK_TIMEOUT_MS 5000
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
typedef unsigned TickType_t;
static atomic_bool g_local_paused=true,g_mqtt_connected=true,g_cache_flush_paused;
static void *g_mqtt_client=(void*)1,*g_offline_ack_queue=(void*)2;
static const char *g_device_sn="SN123",*TAG="test";
static unsigned published,ticks;
static bool pending_ack,pause_after_write;
static unsigned xTaskGetTickCount(void){return ticks;}
static int xQueueReceive(void*q,int *ack,unsigned wait){(void)q;(void)wait;if(!pending_ack)return 0;*ack=17;pending_ack=false;return 1;}
static int esp_mqtt_client_publish(void*c,const char*t,const char*d,int n,int q,int r){(void)c;(void)t;(void)d;(void)n;(void)q;(void)r;published++;pending_ack=true;if(pause_after_write)atomic_store(&g_local_paused,true);return 17;}
FUNCTION
int main(void){
 (void)TAG;assert(!offline_send_callback("alarm","{}",123)&&published==0);
 atomic_store(&g_local_paused,false);pause_after_write=true;
 assert(offline_send_callback("alarm","{}",123)&&published==1); /* Preserve confirmed current item. */
 assert(!offline_send_callback("alarm","{}",124)&&published==1); /* Next item stays queued. */
 atomic_store(&g_local_paused,false);atomic_store(&g_cache_flush_paused,true);
 assert(!offline_send_callback("alarm","{}",125)&&published==1);
 puts("PASS existing flush drains only its in-flight PUBACK and leaves later cached items unsent");
}
'''.replace('FUNCTION',function(mqtt,'offline_send_callback'))
        self.compile_run(harness)

    def test_local_mode_preserves_cache_without_tls_allocations(self):
        mqtt=(ROOT/'main/mqtt/mqtt_handler.c').read_text(encoding='utf-8')
        harness=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define MALLOC_CAP_8BIT 1
static atomic_bool g_local_paused=true,g_mqtt_connected=true;
static void *g_mqtt_client=(void*)1;
static const char *TAG="test",*g_device_sn="SN123";
static unsigned cached,built,published;
static bool offline_cache_add(const char*t,const char*d,unsigned ts){(void)t;(void)d;(void)ts;cached++;return true;}
static int mqtt_envelope_build(const char*d,unsigned ts,unsigned v,char **p,size_t*n){(void)d;(void)ts;(void)v;*p=malloc(8);*n=2;built++;return 0;}
static int esp_mqtt_client_publish(void*c,const char*t,const char*d,int n,int q,int r){(void)c;(void)t;(void)d;(void)n;(void)q;(void)r;published++;return 1;}
static unsigned esp_get_free_heap_size(void){return 80000;}
static unsigned heap_caps_get_largest_free_block(int c){(void)c;return 40000;}
FUNCTION
int main(void){
 (void)TAG;(void)esp_get_free_heap_size;(void)heap_caps_get_largest_free_block;
 assert(publish_core("alarm","{}",1,0,true,1)==1&&cached==1&&!built&&!published);
 assert(publish_core("telemetry","{}",0,0,false,3)==-1&&!built&&!published);
 assert(publish_core(NULL,"{}",0,0,true,1)==-2&&cached==1);
 atomic_store(&g_local_paused,false);
 assert(publish_core("telemetry","{}",0,0,false,3)==0&&built==1&&published==1);
 puts("PASS local pause caches alarms, discards stale telemetry and allocates no TLS envelope");
}
'''.replace('FUNCTION',function(mqtt,'publish_core'))
        self.compile_run(harness)

    def test_manual_phone_page_is_the_intent_source_and_local_pause_gates_watchdog(self):
        panel=(ROOT/'main/direct/direct_panel_runtime.inc').read_text(encoding='utf-8')
        self.assertIn('app_request_local_ble(s_onboarding.visible && !s_onboarding.automatic)',panel)
        mqtt=(ROOT/'main/mqtt/mqtt_handler.c').read_text(encoding='utf-8')
        for name in ('mqtt_is_stuck','mqtt_force_restart','mqtt_flush_offline_cache'):
            self.assertIn('g_local_paused',function(mqtt,name))
        self.assertIn('g_local_paused',mqtt[mqtt.index('static int publish_core('):mqtt.index('void mqtt_clear_retained_status(')])

    def test_mqtt_client_released_only_after_worker_drains_and_rebuilt_once(self):
        source=(ROOT/'main/mqtt/mqtt_handler.c').read_text(encoding='utf-8')
        defines='\n'.join(re.findall(r'^#define MQTT_(?:START_RETRY_MS|TASK_STACK_SIZE|START_RESERVE_SIZE|TRANSPORT_START_TIMEOUT_MS)\s+.*$',source,re.M))
        declarations='\n'.join(re.findall(r'^static (?:atomic_bool g_(?:mqtt_connecting|connect_allowed|local_paused|transport_entered)|atomic_llong g_(?:next_start_ms|connect_attempt_ms|disconnect_start_ms)|void \*g_start_reserve)[^;]*;',source,re.M))
        names=('mqtt_set_connect_allowed','mqtt_connect','mqtt_disconnect','mqtt_pause_for_local','mqtt_resume_after_local','mqtt_is_paused_for_local','mqtt_is_connecting','mqtt_poll_connection')
        harness=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <stdio.h>
DEFINES
DECLARATIONS
#define ESP_OK 0
#define ESP_FAIL -1
typedef int esp_err_t;
static atomic_bool g_mqtt_connected,g_mqtt_started,g_cache_flush_running;
static void *g_mqtt_client=(void*)1,*g_msg_handler_saved;
static int g_binding_subscription_id;
static unsigned clock_ms=1900,starts,reconnects,stops,destroys,inits,forced,freed;
static bool have_ip,fail_start,fail_init;
static const char *TAG="test",*g_mqtt_uri="mqtts://test";
static void test_log(const char *tag,const char *fmt,...){(void)tag;(void)fmt;}
#define ESP_LOGI test_log
#define ESP_LOGW test_log
#define ESP_LOGE test_log
static bool wifi_sta_is_connected(void){return have_ip;}
static int64_t esp_timer_get_time(void){return (int64_t)clock_ms*1000;}
static const char *esp_err_to_name(int e){(void)e;return "error";}
static void mqtt_log_tls_memory(const char *p){(void)p;}
static int esp_mqtt_client_start(void *c){assert(c);starts++;return fail_start?ESP_FAIL:ESP_OK;}
static int esp_mqtt_client_reconnect(void *c){assert(c);reconnects++;return ESP_OK;}
static void heap_caps_free(void *p){if(p){freed++;free(p);}}
static void cloud_binding_subscription(bool p){(void)p;}
static void mqtt_rx_reset(void){}
static void esp_mqtt_client_stop(void *c){assert(c&&!atomic_load(&g_cache_flush_running));stops++;}
static void esp_mqtt_client_destroy(void *c){assert(c);destroys++;}
static bool mqtt_init(void *handler){(void)handler;inits++;if(fail_init)return false;g_mqtt_client=(void*)1;return true;}
static void mqtt_force_restart(void){forced++;atomic_store(&g_mqtt_connecting,false);atomic_store(&g_mqtt_started,false);}
FUNCTIONS
int main(void){
 g_start_reserve=malloc(MQTT_START_RESERVE_SIZE);assert(g_start_reserve);
 assert(!mqtt_connect()&&starts==0&&freed==0&&g_next_start_ms==0);
 have_ip=true;assert(!mqtt_connect()&&starts==0);
 mqtt_set_connect_allowed(true);assert(mqtt_connect()&&starts==1&&freed==1);
 assert(mqtt_is_connecting()&&!mqtt_connect()&&reconnects==0);
 atomic_store(&g_transport_entered,true);clock_ms+=100000;mqtt_poll_connection();assert(!forced);
 atomic_store(&g_mqtt_connecting,false);g_next_start_ms=clock_ms+MQTT_START_RETRY_MS;
 assert(!mqtt_connect()&&!reconnects);clock_ms+=MQTT_START_RETRY_MS;
 assert(mqtt_connect()&&reconnects==1&&starts==1);
 atomic_store(&g_transport_entered,true);atomic_store(&g_mqtt_connecting,false);atomic_store(&g_mqtt_connected,true);
 assert(mqtt_connect()&&starts==1&&reconnects==1);
 atomic_store(&g_cache_flush_running,true);
 assert(!mqtt_pause_for_local()&&mqtt_is_paused_for_local()&&!stops&&!destroys);
 assert(!mqtt_connect());
 atomic_store(&g_cache_flush_running,false);assert(mqtt_pause_for_local());
 assert(stops==1&&destroys==1&&!g_mqtt_client&&!atomic_load(&g_mqtt_started));
 fail_init=true;assert(!mqtt_resume_after_local()&&mqtt_is_paused_for_local());
 fail_init=false;assert(mqtt_resume_after_local()&&!mqtt_is_paused_for_local());
 unsigned old_inits=inits;assert(mqtt_resume_after_local()&&inits==old_inits);
 mqtt_set_connect_allowed(true);assert(mqtt_connect()&&starts==2);
 have_ip=false;clock_ms+=MQTT_TRANSPORT_START_TIMEOUT_MS+1;mqtt_poll_connection();
 assert(forced==1&&!mqtt_is_connecting());
 have_ip=true;clock_ms+=MQTT_START_RETRY_MS;fail_start=true;
 assert(!mqtt_connect()&&starts==3);assert(!mqtt_connect()&&starts==3);
 clock_ms+=MQTT_START_RETRY_MS;fail_start=false;assert(mqtt_connect()&&starts==4);
 atomic_store(&g_mqtt_connecting,false);assert(mqtt_pause_for_local());assert(mqtt_resume_after_local());
 puts("PASS DHCP, 8KB reserve, retry, worker drain, destroy/rebuild, silent exit and allocation backoff");
}
'''
        self.compile_run(harness.replace('DEFINES',defines).replace('DECLARATIONS',declarations).replace('FUNCTIONS','\n'.join(function(source,n) for n in names)))


    def compile_run(self, harness):
        with tempfile.TemporaryDirectory(prefix='mqtt_local_') as folder:
            path=Path(folder)
            source,exe=path/'test.c',path/'test.exe'
            source.write_text(harness,encoding='utf-8')
            env=os.environ.copy()
            env['PATH']=str(Path(GCC).parent)+os.pathsep+env.get('PATH','')
            subprocess.run([GCC,'-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable',str(source),'-o',str(exe)],check=True,env=env,timeout=30)
            result=subprocess.run([str(exe)],capture_output=True,text=True,env=env,timeout=10)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            print(result.stdout.strip())


if __name__=='__main__':
    unittest.main()
