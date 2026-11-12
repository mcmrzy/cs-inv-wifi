"""Compile real publish admission and replay callback; preserve V3 and original time."""
from pathlib import Path
import os,re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
GCC=os.environ.get("HOST_GCC",r"C:\Users\29538\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe")
source=(ROOT/"main/mqtt/mqtt_handler.c").read_text(encoding="utf-8")
def extract(name):
    m=re.search(r"^(?:static )?(?:bool|int)\s+"+name+r"\([^;]*?\)\s*\{",source,re.M);assert m,name
    end=source.index("{",m.start())+1;depth=1
    while depth:
        depth+=(source[end]=="{")-(source[end]=="}");end+=1
    return source[m.start():end]
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include "offline_cache_route.h"
#define PROTO_TOPIC_FMT "cs_inv/%s/%s"
#define PROTO_HEARTBEAT_ENVELOPE_VERSION 3
#define PROTO_ENVELOPE_VERSION 2
#define OFFLINE_ACK_TIMEOUT_MS 5000
#define MALLOC_CAP_8BIT 1
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
typedef unsigned TickType_t;
static atomic_bool g_local_paused,g_cache_flush_paused,g_mqtt_connected;
static void *g_mqtt_client=(void*)1,*g_offline_ack_queue=(void*)1;
static const char *g_device_sn="TEST";
static char cached_topic[64],cached_payload[1024],wire_topic[128],wire_payload[1152];
static bool ack_pending,ack_allowed=true,in_replay;
static int enqueue_result=77;
static bool offline_cache_add(const char *t,const char *p,uint32_t timestamp) {
    assert(timestamp==0 || timestamp==1700000000);
    assert(strlen(t)<sizeof(cached_topic) && strlen(p)<sizeof(cached_payload));
    strcpy(cached_topic,t);strcpy(cached_payload,p);return true;
}
#define esp_get_free_heap_size() 99999
#define heap_caps_get_largest_free_block(cap) 99999
static int mqtt_envelope_build(const char *d,uint32_t t,uint16_t v,char **p,size_t *n){
    (void)d;(void)t;(void)v;(void)p;(void)n;return -1;
}
static int esp_mqtt_client_publish(void *client,const char *t,const char *p,int len,int qos,int retain) {
    assert(!in_replay); /* Synchronous TLS publishing must never use the replay task. */
    assert(client==g_mqtt_client && qos==1 && retain==0 && len==(int)strlen(p));
    strcpy(wire_topic,t);strcpy(wire_payload,p);return 77;
}
static int esp_mqtt_client_enqueue(void *client,const char *t,const char *p,int len,int qos,int retain,bool store) {
    assert(in_replay && store && client==g_mqtt_client && qos==1 && !retain);
    assert(len==(int)strlen(p));
    if(enqueue_result<0)return enqueue_result;
    strcpy(wire_topic,t);strcpy(wire_payload,p);ack_pending=ack_allowed;return enqueue_result;
}
static TickType_t xTaskGetTickCount(void){return 1;}
static int xQueueReceive(void *q,int *id,unsigned wait){
    assert(q==g_offline_ack_queue);
    if(!wait || !ack_pending)return 0;
    *id=77;ack_pending=false;return pdTRUE;
}
FUNCTIONS
static bool replay(const char *topic,const char *payload,uint32_t timestamp) {
    in_replay=true;bool ok=offline_send_callback(topic,payload,timestamp);in_replay=false;return ok;
}
int main(void){
    char payload[1024];memset(payload,'x',sizeof(payload));
    memcpy(payload,"{\"run\":\"",8);payload[1000]='\"';payload[1001]='}';payload[1002]=0;
    atomic_store(&g_mqtt_connected,false);
    assert(mqtt_publish_heartbeat_raw("heartbeat",payload,1002)==1);
    assert(strcmp(cached_topic,"~3/heartbeat")==0 && strcmp(cached_payload,payload)==0);
    atomic_store(&g_mqtt_connected,true);
    assert(replay(cached_topic,cached_payload,1700000000));
    assert(strcmp(wire_topic,"cs_inv/TEST/heartbeat")==0);
    assert(strstr(wire_payload,"\"t\":1700000000,\"v\":3,\"data\":") && strstr(wire_payload,payload));
    assert(replay("config","{}",1700000000));assert(strstr(wire_payload,"\"v\":2"));
    enqueue_result=-2;assert(!replay("~3/heartbeat",payload,1700000000));enqueue_result=77;
    ack_allowed=false;assert(!replay("~3/heartbeat",payload,1700000000));
    atomic_store(&g_cache_flush_paused,true);assert(!replay("config","{}",1700000000));
    puts("PASS real V3 offline admission and replay: 1002-byte payload, original time/version/topic, legacy V2, SDK outbox only, queue full, missing ACK and OTA pause");
    return 0;
}
'''
code=code.replace("FUNCTIONS","\n".join(extract(n) for n in ("offline_send_callback","cache_message","publish_core","mqtt_publish_heartbeat_raw")))
with tempfile.TemporaryDirectory(prefix="offline-heartbeat-") as tmp:
    tmp=Path(tmp);c=tmp/"test.c";c.write_text(code,encoding="utf-8");exe=tmp/"test.exe"
    subprocess.run([GCC,"-std=c11","-Wall","-Wextra","-Werror","-Wno-unused-function","-save-temps=obj","-I",str(ROOT/"main/storage"),str(c),"-o",str(exe)],check=True,cwd=tmp)
    subprocess.run([str(exe)],check=True,cwd=tmp)
