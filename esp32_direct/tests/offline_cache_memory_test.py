"""Run real cache entry points against a batch-persistence boundary.

The flush double preserves the production postcondition: failure retains the
whole RAM batch; success persists its contents, frees it and sets it to NULL.
This tests allocation/ownership, not cJSON encoding or SPIFFS durability.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
GCC = os.environ.get(
    "HOST_GCC",
    r"C:\Users\29538\AppData\Local\Microsoft\WinGet\Packages"
    r"\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe"
    r"\mingw64\bin\gcc.exe",
)


def function(source, name):
    match = re.search(
        r"^(?:static\s+)?(?:bool|int)\s+" + name + r"\([^;]*?\)\s*\{",
        source, re.M,
    )
    if not match:
        raise AssertionError("Missing cache function: " + name)
    end = source.index("{", match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


HARNESS = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c); exit(2); } } while (0)
#define ESP_OK 0
#define ESP_FAIL -1
#define pdTRUE 1
#define portMAX_DELAY 999999U
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
__BATCH_DEFINE__
__ITEM_TYPE__
static bool s_initialized=true;
static void *s_mutex=(void*)1;
static OfflineCacheItem *s_batch;
static int s_batch_count;
static int64_t s_batch_start_ms;
static uint32_t s_total_msgs,s_last_sync;
static bool held,external_owner,fail_take,fail_flush,fail_alloc;
static unsigned allocations,flushes,gives;
static OfflineCacheItem persisted[100];
static unsigned persisted_count;
struct GuardedBatch { uint64_t before; OfflineCacheItem items[MAX_BATCH_ITEMS]; uint64_t after; };
static struct GuardedBatch *allocation;
#define CANARY UINT64_C(0x9876543210abcdef)
static void check_guards(void) {
    if (allocation) CHECK(allocation->before==CANARY && allocation->after==CANARY);
    CHECK(s_batch_count>=0 && s_batch_count<=MAX_BATCH_ITEMS);
}
static int xSemaphoreTake(void *mutex,unsigned timeout) {
    (void)timeout; CHECK(mutex==s_mutex);
    if (fail_take) return 0;
    CHECK(!held && !external_owner); held=true; return pdTRUE;
}
static void xSemaphoreGive(void *mutex) {
    CHECK(mutex==s_mutex && held && !external_owner); held=false; gives++;
}
static int64_t now_ms(void) { return 123456; }
static uint32_t now_epoch(void) { return 1700000000U; }
static void *batch_calloc(size_t count,size_t size) {
    CHECK(held && !external_owner && !allocation);
    CHECK(count==MAX_BATCH_ITEMS && size==sizeof(OfflineCacheItem));
    allocations++;
    if (fail_alloc) return NULL;
    allocation=calloc(1,sizeof(*allocation)); CHECK(allocation);
    allocation->before=allocation->after=CANARY;
    return allocation->items;
}
static void *checked_memset(void *dest,int value,size_t length) {
    CHECK(dest!=NULL); check_guards(); return memset(dest,value,length);
}
static int flush_batch(void) {
    CHECK(held && !external_owner); check_guards(); flushes++;
    if (fail_flush) return ESP_FAIL;
    CHECK(s_batch && persisted_count+(unsigned)s_batch_count<=100);
    memcpy(persisted+persisted_count,s_batch,s_batch_count*sizeof(*s_batch));
    persisted_count+=(unsigned)s_batch_count;
    s_total_msgs+=(unsigned)s_batch_count;
    free(allocation); allocation=NULL; s_batch=NULL; s_batch_count=0;
    return ESP_OK;
}
#define calloc batch_calloc
#define memset checked_memset
__ADD__
#undef calloc
#undef memset

/* Only the initial mutex-acquisition failure is injected for flush_all.
 * No file/JSON operation may be reached without owning the mutex. */
typedef struct cJSON { char *valuestring; double valuedouble; } cJSON;
static uint32_t find_min_seq(void) { CHECK(held&&!external_owner); return UINT32_MAX; }
static void make_filename(uint32_t seq,char *path,size_t cap) { (void)seq;(void)path;(void)cap; CHECK(false); }
static cJSON *read_batch_file(const char *path) { (void)path; CHECK(false); return NULL; }
static int cJSON_GetArraySize(cJSON *arr) { (void)arr; CHECK(false); return 0; }
static cJSON *cJSON_GetArrayItem(cJSON *arr,int i) { (void)arr;(void)i; CHECK(false); return NULL; }
static cJSON *cJSON_GetObjectItem(cJSON *arr,const char *key) { (void)arr;(void)key; CHECK(false); return NULL; }
static cJSON *cJSON_CreateArray(void) { CHECK(false); return NULL; }
static cJSON *cJSON_Duplicate(cJSON *item,bool recurse) { (void)item;(void)recurse; CHECK(false); return NULL; }
static void cJSON_AddItemToArray(cJSON *arr,cJSON *item) { (void)arr;(void)item; CHECK(false); }
static void cJSON_Delete(cJSON *arr) { (void)arr; CHECK(false); }
static char *cJSON_PrintUnformatted(cJSON *arr) { (void)arr; CHECK(false); return NULL; }
static unsigned count_all_messages(void) { CHECK(held&&!external_owner); return persisted_count; }
static void save_meta(void) { CHECK(held&&!external_owner); }
static int test_fileno(FILE *file) { (void)file; CHECK(false); return -1; }
static int test_fsync(int fd) { (void)fd; CHECK(false); return -1; }
#define fileno test_fileno
#define fsync test_fsync
__FLUSH_ALL__

static bool add(unsigned id) {
    char payload[32]; snprintf(payload,sizeof(payload),"{\"id\":%u}",id);
    bool result=offline_cache_add("alarm",payload,id+100);
    CHECK(!held); check_guards(); return result;
}
static void fill_failed_batch(void) {
    fail_flush=true;
    for (unsigned i=0;i<MAX_BATCH_ITEMS;i++) CHECK(add(i)==(i+1<MAX_BATCH_ITEMS));
    CHECK(s_batch && s_batch_count==MAX_BATCH_ITEMS && persisted_count==0);
}
static void verify_persisted(unsigned count) {
    CHECK(persisted_count==count && s_total_msgs==count);
    for (unsigned i=0;i<count;i++) {
        char payload[32]; snprintf(payload,sizeof(payload),"{\"id\":%u}",i);
        CHECK(!strcmp(persisted[i].topic,"alarm") && !strcmp(persisted[i].payload,payload));
        CHECK(persisted[i].timestamp==i+100 && persisted[i].index==i%MAX_BATCH_ITEMS);
    }
}
static bool send_item(const char *topic,const char *payload,uint32_t timestamp) {
    (void)topic;(void)payload;(void)timestamp; CHECK(false); return false;
}
int main(int argc,char **argv) {
    CHECK(argc==2);
    if (!strcmp(argv[1],"retry")) {
        fill_failed_batch(); fail_flush=false;
        CHECK(add(10)); verify_persisted(10);
        CHECK(allocations==2 && s_batch_count==1 && s_batch);
        CHECK(!strcmp(s_batch[0].payload,"{\"id\":10}") && s_batch[0].index==0);
    } else if (!strcmp(argv[1],"alloc_after_flush")) {
        fill_failed_batch(); fail_flush=false; fail_alloc=true;
        CHECK(!add(10)); verify_persisted(10);
        CHECK(allocations==2 && !s_batch && s_batch_count==0);
        fail_alloc=false; CHECK(add(10)); verify_persisted(10);
        CHECK(s_batch && s_batch_count==1 && !strcmp(s_batch[0].payload,"{\"id\":10}"));
    } else if (!strcmp(argv[1],"failed_flush")) {
        fill_failed_batch(); OfflineCacheItem before[MAX_BATCH_ITEMS];
        memcpy(before,s_batch,sizeof(before));
        for (unsigned i=0;i<20;i++) {
            CHECK(!add(10)); CHECK(allocations==1 && s_batch_count==MAX_BATCH_ITEMS);
            CHECK(!memcmp(before,s_batch,sizeof(before)) && persisted_count==0);
        }
    } else if (!strcmp(argv[1],"normal")) {
        for (unsigned i=0;i<30;i++) {
            CHECK(add(i));
            if ((i+1)%MAX_BATCH_ITEMS==0) CHECK(!s_batch && !allocation && s_batch_count==0);
            else CHECK(s_batch && s_batch_count==(int)((i+1)%MAX_BATCH_ITEMS));
        }
        CHECK(allocations==3 && flushes==3); verify_persisted(30);
    } else if (!strcmp(argv[1],"initial_alloc")) {
        fail_alloc=true; CHECK(!add(0));
        CHECK(!s_batch && s_batch_count==0 && !persisted_count && !flushes);
        fail_alloc=false; CHECK(add(0)); CHECK(s_batch_count==1);
    } else if (!strcmp(argv[1],"flush_lock")) {
        fill_failed_batch(); unsigned before_flushes=flushes,before_gives=gives;
        fail_take=true; external_owner=true;
        CHECK(offline_cache_flush_all(send_item)==0);
        CHECK(flushes==before_flushes && gives==before_gives && external_owner);
        CHECK(s_batch_count==MAX_BATCH_ITEMS && !persisted_count);
    } else CHECK(false);
    check_guards(); free(allocation);
    puts("PASS"); return 0;
}
'''


class OfflineCacheMemoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (ROOT / "main/storage/offline_cache.c").read_text(encoding="utf-8")
        header = (ROOT / "main/storage/offline_cache.h").read_text(encoding="utf-8")
        batch_define = re.search(r"^#define MAX_BATCH_ITEMS\s+.*$", source, re.M)
        item_type = re.search(r"typedef struct\s*\{.*?\}\s*OfflineCacheItem;", header, re.S)
        harness = HARNESS.replace("__BATCH_DEFINE__", batch_define.group())
        harness = harness.replace("__ITEM_TYPE__", item_type.group())
        harness = harness.replace("__ADD__", function(source, "offline_cache_add"))
        harness = harness.replace("__FLUSH_ALL__", function(source, "offline_cache_flush_all"))
        cls.folder = tempfile.TemporaryDirectory(prefix="offline_cache_memory_")
        cls.addClassCleanup(cls.folder.cleanup)
        path = Path(cls.folder.name)
        cls.exe = path / "cache_test.exe"
        target = path / "cache_test.c"
        target.write_text(harness, encoding="utf-8")
        cls.env = os.environ.copy()
        cls.env["PATH"] = str(Path(GCC).parent) + os.pathsep + cls.env.get("PATH", "")
        subprocess.run(
            [GCC, "-std=c11", "-Wall", "-Wextra", "-Werror", str(target), "-o", str(cls.exe)],
            check=True, env=cls.env, timeout=30,
        )

    def run_case(self, case):
        result = subprocess.run(
            [str(self.exe), case], capture_output=True, text=True,
            env=self.env, timeout=10,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_full_batch_retry_reallocates_after_flush_releases_ram(self):
        self.run_case("retry")

    def test_allocation_failure_after_flush_preserves_old_messages(self):
        self.run_case("alloc_after_flush")

    def test_repeated_failed_flush_retains_full_batch_without_overflow(self):
        self.run_case("failed_flush")

    def test_normal_ten_message_batches_still_flush_and_release(self):
        self.run_case("normal")

    def test_initial_allocation_failure_can_retry(self):
        self.run_case("initial_alloc")

    def test_flush_all_lock_timeout_does_not_touch_another_owner_batch(self):
        self.run_case("flush_lock")


if __name__ == "__main__":
    unittest.main(verbosity=2)
