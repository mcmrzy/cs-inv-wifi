"""Exercise production codec with real cJSON, including old batches and corrupt deltas."""
from pathlib import Path
import os
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
GCC = os.environ.get("HOST_GCC", r"C:\Users\29538\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe")
CJSON = Path(os.environ.get("IDF_PATH", "D:/.espressif/v5.5.4/esp-idf")) / "components/json/cJSON"
HARNESS = r''' 
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "offline_cache_codec.h"
static cJSON *row(const char *p) {
    cJSON *r=cJSON_CreateObject();assert(r);
    assert(cJSON_AddStringToObject(r,"t","telemetry"));
    assert(cJSON_AddStringToObject(r,"p",p));
    assert(cJSON_AddNumberToObject(r,"ts",1700000000));return r;
}
static void *fail_malloc(size_t n) { (void)n; return NULL; }
int main(void) {
    size_t original=0,compressed=0;
    srand(42);
    for(int trial=0;trial<500;++trial) {
        cJSON *a=cJSON_CreateArray();char samples[10][1024];
        for(int j=0;j<10;++j) {
            snprintf(samples[j],1024,"{\"battery_voltage\":26.70,\"battery_current\":%d,\"soc\":80,\"mode\":\"battery\",\"temperature\":25}",rand()%100);
            if(trial%7==0) { /* UTF-8, escapes, boundary sizes and unchanged messages */
                memset(samples[j],'x',1023);samples[j][1023]=0;
                if(j%2) samples[j][trial%1023]='y';
            }
            if(trial%11==0) snprintf(samples[j],1024,"{\"battery_name\":\"电池协议\",\"note\":\"quoted \\\"x\\\"\",\"temperature\":%d,\"tail\":\"充电上限保持\"}",j);
            cJSON_AddItemToArray(a,row(samples[j]));
        }
        char *plain=cJSON_PrintUnformatted(a);original+=strlen(plain);
        assert(cache_batch_decode(a)); /* old plain cache */
        assert(cache_batch_encode(a));
        char *packed=cJSON_PrintUnformatted(a);compressed+=strlen(packed);
        cJSON_Delete(a);a=cJSON_Parse(packed);assert(a && cache_batch_decode(a));
        char *result=cJSON_PrintUnformatted(a);assert(strcmp(plain,result)==0);
        for(int j=0;j<10;++j) assert(strcmp(samples[j],cJSON_GetObjectItem(cJSON_GetArrayItem(a,j),"p")->valuestring)==0);
        free(plain);free(packed);free(result);cJSON_Delete(a);
    }
    char first[200],second[200];memset(first,'a',199);first[199]=0;
    strcpy(second,first);second[100]='b';
    cJSON *memory=cJSON_CreateArray();cJSON_AddItemToArray(memory,row(first));cJSON_AddItemToArray(memory,row(second));
    cJSON_Hooks hooks={fail_malloc,free};cJSON_InitHooks(&hooks);
    assert(!cache_batch_encode(memory));cJSON_InitHooks(NULL);
    assert(!strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(memory,1),"p")->valuestring,second));
    assert(cache_batch_encode(memory));cJSON_InitHooks(&hooks);
    assert(!cache_batch_decode(memory));cJSON_InitHooks(NULL);
    assert(cache_batch_decode(memory));
    assert(!strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(memory,1),"p")->valuestring,second));cJSON_Delete(memory);
    const char *bad[]={
      "[{\"t\":\"x\",\"p\":\"x\",\"k\":1,\"e\":0}]",
      "[{\"t\":\"x\",\"p\":\"x\"},{\"t\":\"x\",\"p\":\"x\",\"k\":1024,\"e\":0}]",
      "[{\"t\":\"x\",\"p\":\"x\"},{\"t\":\"x\",\"p\":\"x\",\"k\":0.5,\"e\":0}]",
      "[{\"t\":\"x\",\"p\":3}]"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);++i) {
        cJSON *a=cJSON_Parse(bad[i]);assert(a);assert(!cache_batch_decode(a));cJSON_Delete(a);
    }
    cJSON *utf=cJSON_CreateArray();cJSON_AddItemToArray(utf,row("电池\\协议\"测试\""));
    cJSON_AddItemToArray(utf,row("电池\\协议\"更新\""));assert(cache_batch_encode(utf));assert(cache_batch_decode(utf));cJSON_Delete(utf);
    printf("PASS 5000 lossless records, old format, boundaries, corrupt deltas; bytes %zu -> %zu\n",original,compressed);
    return 0;
}
'''
def production_function(name):
    source=(ROOT/"main/storage/offline_cache.c").read_text(encoding="utf-8")
    import re
    match=re.search(r"^(?:static )?(?:bool|void|esp_err_t|cJSON \*)\s*"+name+r"\([^;]*?\)\s*\{", source,re.M)
    assert match,name
    end=source.index("{",match.start())+1;depth=1
    while depth:
        depth+=(source[end]=="{")-(source[end]=="}");end+=1
    return source[match.start():end]
INTEGRATION = r'''
#include <stdint.h>
#include <unistd.h>
#include <io.h>
#include <sys/stat.h>
#include <errno.h>
#define fsync _commit
#define SPIFFS_MOUNT "."
#define MAX_BATCH_ITEMS 5
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM -2
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
typedef int esp_err_t;
typedef struct { char topic[64],payload[1024];uint32_t timestamp,index; } OfflineCacheItem;
static OfflineCacheItem *s_batch;
static int s_batch_count;
static uint32_t s_next_seq=1,s_total_msgs;
static void evict_if_needed(void) {}
static void evict_oldest_batch(void) {}
static void save_meta(void) {}
static bool fail_rename;
static int checked_rename(const char *from,const char *to) {
    return fail_rename ? -1 : rename(from,to);
}
#define rename checked_rename
'''
INTEGRATION += "\n#define access(path,mode) (errno=ENOSYS,-1)\n"
INTEGRATION += "\n".join(production_function(n) for n in ("make_filename","batch_name","read_batch_file","flush_batch"))
INTEGRATION += "\n#undef access\n"
INTEGRATION += r'''
static void storage_test(void) {
    assert(batch_name("b00000001") && batch_name("c00000001"));
    assert(!batch_name("c00000001.tmp") && !batch_name("cat"));
    s_batch=calloc(MAX_BATCH_ITEMS,sizeof(*s_batch));assert(s_batch);s_batch_count=MAX_BATCH_ITEMS;
    for(int i=0;i<MAX_BATCH_ITEMS;++i) {
        strcpy(s_batch[i].topic,"telemetry");
        snprintf(s_batch[i].payload,512,"{\"battery_voltage\":26.70,\"battery_current\":%d,\"soc\":80,\"mode\":\"battery\"}",i);
        s_batch[i].timestamp=1700000000+i;
    }
    fail_rename=true;assert(flush_batch()!=ESP_OK);
    assert(s_batch && s_batch_count==MAX_BATCH_ITEMS && s_total_msgs==0 && s_next_seq==1);
    assert(access("c00000001",F_OK)!=0);
    fail_rename=false;assert(flush_batch()==ESP_OK);
    assert(!s_batch && s_batch_count==0 && s_total_msgs==MAX_BATCH_ITEMS && s_next_seq==2);
    assert(access("c00000001.tmp",F_OK)!=0);
    char path[280];make_filename(1,path,sizeof(path));cJSON *a=read_batch_file(path);
    assert(a && cJSON_GetArraySize(a)==MAX_BATCH_ITEMS);
    for(int i=0;i<MAX_BATCH_ITEMS;++i) {
        char expected[512];snprintf(expected,512,"{\"battery_voltage\":26.70,\"battery_current\":%d,\"soc\":80,\"mode\":\"battery\"}",i);
        cJSON *r=cJSON_GetArrayItem(a,i);
        assert(strcmp(cJSON_GetObjectItem(r,"p")->valuestring,expected)==0);
        assert(cJSON_GetObjectItem(r,"ts")->valuedouble==1700000000+i);
    }
    cJSON_Delete(a);remove("c00000001");
    FILE *f=fopen("b00000001","w");assert(f);
    fputs("[{\"t\":\"telemetry\",\"p\":\"{}\",\"ts\":1700000000}]",f);fclose(f);
    make_filename(1,path,sizeof(path));a=read_batch_file(path);assert(a);cJSON_Delete(a);remove("b00000001");
    puts("PASS production file commit, rename failure retention, exact timestamps, old files and temporary-file exclusion");
}
'''
HARNESS=HARNESS.replace("int main(void) {",INTEGRATION+"\nint main(void) {\n    storage_test();")
with tempfile.TemporaryDirectory(prefix="cache-codec-") as tmp:
    tmp=Path(tmp); source=tmp/"codec.c";source.write_text(HARNESS,encoding="utf-8")
    exe=tmp/"codec.exe"
    subprocess.run([GCC,"-std=c11","-Wall","-Wextra","-Werror","-save-temps=obj","-I",str(ROOT/"main/storage"),"-I",str(CJSON),str(source),str(CJSON/"cJSON.c"),"-o",str(exe)],check=True,cwd=tmp)
    subprocess.run([str(exe)],check=True,cwd=tmp)
