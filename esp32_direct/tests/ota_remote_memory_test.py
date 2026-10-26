"""Execute real OTA admission/coordinator functions with a fragmented heap."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GCC = Path(r"C:\Users\29538\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe")


def function(source, name):
    match = re.search(r"^[\w *]+\b" + name + r"\([^;]*?\)\s*\{", source, re.M)
    assert match, name
    start, end = match.start(), match.end()
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class RemoteMemoryTests(unittest.TestCase):
    def test_fragmented_heap_and_recovery(self):
        source = (ROOT / "main/ota/ota_service.c").read_text(encoding="utf-8")
        self.assertTrue("xTaskCreateStatic(remote_coordinator" in source,
                      "OTA coordinator must be reserved before BLE fragments the heap")
        self.assertIn("start_remote_coordinator()", function(source, "ota_service_init"))
        defines = "\n".join(re.findall(r"^#define OTA_(?:TASK_STACK_SIZE|TASK_PRIORITY|ALLOC_ATTEMPTS|ALLOC_RETRY_MS|COORDINATOR_STACK_SIZE|COORDINATOR_PRIORITY)\s+.*$", source, re.M))
        harness = r'''
#include "ota_service.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#define pdPASS 1
#define pdTRUE 1
#define portMAX_DELAY 0xffffffffU
#define pdMS_TO_TICKS(x) (x)
#define MALLOC_CAP_8BIT 1
static void fake_log(const char*t,const char*f,...){(void)t;(void)f;}
#define ESP_LOGW(...) fake_log(__VA_ARGS__)
#define ESP_LOGE(...) fake_log(__VA_ARGS__)
#define ESP_LOGI(...) fake_log(__VA_ARGS__)
typedef void (*TaskFunction_t)(void*);
typedef void *TaskHandle_t;
typedef uint32_t StackType_t;
typedef struct { unsigned placeholder; } StaticTask_t;
static void *g_lock=(void*)1;
static TaskHandle_t g_remote_task, g_remote_coordinator;
static bool g_remote_cleanup_busy, g_remote_reboot_pending;
static OtaManifest g_manifest;
static bool active, ble_stopped, suspend_ok, restore_ok, cancel, fail_worker, reboot_worker;
static bool network_quiet, cache_quiet, flush_paused;
static unsigned flush_busy_cycles;
static unsigned largest, failures, restores, worker_deletes, quiet_on, quiet_off;
static unsigned static_creates, dynamic_creates, notifications, auth_calls, reserve_calls;
static OtaResult last_failure, admission_result;
static TaskFunction_t queued;
static jmp_buf idle;
static unsigned heap_caps_get_free_size(unsigned c){(void)c;return 16000;}
static unsigned heap_caps_get_largest_free_block(unsigned c){(void)c;return largest;}
static void vTaskDelay(unsigned n){(void)n;if(flush_busy_cycles)flush_busy_cycles--;}
static void telemetry_set_quiet(bool q){network_quiet=q;if(q)quiet_on++;else quiet_off++;}
static void offline_cache_set_quiet(bool q){cache_quiet=q;}
static bool mqtt_set_cache_flush_paused(bool p){flush_paused=p;return !flush_busy_cycles;}
static int xSemaphoreTake(void*l,unsigned n){assert(l);(void)n;return pdTRUE;}
static void xSemaphoreGive(void*l){assert(l);}
static TaskHandle_t xTaskCreateStatic(TaskFunction_t f,const char*n,unsigned stack,void*p,unsigned prio,StackType_t*s,StaticTask_t*t){
 (void)p;(void)prio;assert(!strcmp(n,"ota_prepare")&&stack==4096&&s&&t);
 static_creates++;queued=f;return (void*)2;
}
static int xTaskCreate(TaskFunction_t f,const char*n,unsigned stack,void*p,unsigned prio,TaskHandle_t*h){
 (void)f;(void)p;(void)prio;dynamic_creates++;
 assert(!strcmp(n,"ota_remote")&&ble_stopped&&stack==12288);
 assert(network_quiet&&cache_quiet&&flush_paused&&!flush_busy_cycles);
 if(stack>largest||fail_worker)return 0;
 largest-=stack;if(h)*h=(void*)3;return pdPASS;
}
static void vTaskDelete(void*h){assert(h==(void*)3);worker_deletes++;largest+=12288;}
static void xTaskNotifyGive(TaskHandle_t h){assert(h==(void*)2);notifications++;}
static unsigned ulTaskNotifyTake(int clear,unsigned wait){
 assert(clear&&wait==portMAX_DELAY);
 if(g_remote_task){active=false;g_remote_reboot_pending=reboot_worker;return 1;}
 if(notifications){notifications=0;return 1;}
 longjmp(idle,1);
}
static bool ble_prov_suspend_for_ota(void){if(!suspend_ok)return false;ble_stopped=true;largest=50000;return true;}
static bool ble_prov_restore_after_ota(void){
 assert(!g_remote_task);if(ble_stopped)assert(largest>=12288);restores++;
 assert(g_remote_cleanup_busy);
 OtaManifest m={.target=OTA_TARGET_DSP};assert(ota_service_start_remote(&m)==OTA_RESULT_BUSY);
 if(restore_ok)ble_stopped=false;
 return restore_ok;
}
static void mqtt_publish_ota_trace(const char*a,const char*b){assert(a&&b);}
QUIET_FUNCTIONS
static bool cancel_requested(void){return cancel;}
static OtaResult validate_manifest_cheap(const OtaManifest*m){assert(m);return OTA_RESULT_OK;}
static OtaResult check_available(const OtaManifest*m){(void)m;return active||g_remote_cleanup_busy?OTA_RESULT_BUSY:admission_result;}
static OtaResult authenticate_manifest(const OtaManifest*m){(void)m;auth_calls++;return OTA_RESULT_OK;}
static OtaResult reserve_manifest(const OtaManifest*m){g_manifest=*m;active=true;reserve_calls++;return OTA_RESULT_OK;}
static void arm_ota_workspace_release(void){}
static void finish_failure(OtaResult r,const char*m){assert(m);failures++;last_failure=r;active=false;}
static void remote_worker(void*p){(void)p;}
DEFINES
STORAGE
_Static_assert(sizeof(g_remote_coordinator_stack)==4096,"Coordinator stack must be exactly 4096 bytes");
static void remote_coordinator(void* parameter);
INIT
CREATE
COORDINATOR
START
static void reset(void){
 active=ble_stopped=cancel=fail_worker=reboot_worker=g_remote_cleanup_busy=g_remote_reboot_pending=false;
 suspend_ok=restore_ok=true;largest=50000;
 network_quiet=cache_quiet=flush_paused=false;flush_busy_cycles=2;
 failures=restores=worker_deletes=quiet_on=quiet_off=static_creates=dynamic_creates=notifications=auth_calls=reserve_calls=0;
 admission_result=OTA_RESULT_OK;g_remote_task=g_remote_coordinator=NULL;queued=NULL;
 assert(start_remote_coordinator()&&g_remote_coordinator&&static_creates==1);
 assert(start_remote_coordinator()&&static_creates==1);
 largest=2000;
}
static void launch(OtaManifest*m){
 unsigned before=dynamic_creates;assert(ota_service_start_remote(m)==OTA_RESULT_OK);
 assert(g_remote_cleanup_busy&&notifications==1&&dynamic_creates==before);
 assert(ota_service_start_remote(m)==OTA_RESULT_BUSY);
}
static void run_job(void){if(!setjmp(idle))queued(NULL);assert(g_remote_coordinator==(void*)2&&static_creates==1);}
int main(void){
 (void)telemetry_set_quiet;(void)offline_cache_set_quiet;(void)mqtt_set_cache_flush_paused;
 (void)ota_quiet_end;
 OtaManifest m={.target=OTA_TARGET_ESP};
 reset();launch(&m);run_job();assert(worker_deletes==1&&restores==1&&!ble_stopped&&!g_remote_cleanup_busy&&quiet_on==quiet_off);
 assert(!network_quiet&&!cache_quiet&&!flush_paused);
 largest=1000;launch(&m);run_job();assert(worker_deletes==2&&restores==2&&quiet_on==quiet_off);
 reset();fail_worker=true;launch(&m);run_job();assert(failures==1&&last_failure==OTA_RESULT_TASK_CREATE_FAILED&&restores==1&&!g_remote_cleanup_busy&&quiet_on==quiet_off);
 fail_worker=false;largest=1500;launch(&m);run_job();assert(worker_deletes==1&&restores==2&&!g_remote_cleanup_busy);
 reset();suspend_ok=false;launch(&m);run_job();assert(failures==1&&!worker_deletes&&!g_remote_cleanup_busy&&quiet_on==quiet_off);
 suspend_ok=true;launch(&m);run_job();assert(worker_deletes==1&&restores==2);
 reset();cancel=true;launch(&m);run_job();assert(failures==1&&last_failure==OTA_RESULT_OK&&!worker_deletes&&quiet_on==quiet_off);
 cancel=false;launch(&m);run_job();assert(worker_deletes==1&&restores==2);
 reset();restore_ok=false;launch(&m);run_job();assert(worker_deletes==1&&restores==1&&!g_remote_cleanup_busy);
 restore_ok=true;launch(&m);run_job();assert(worker_deletes==2&&restores==2&&!ble_stopped);
 reset();reboot_worker=true;launch(&m);run_job();assert(worker_deletes==1&&restores==0&&ble_stopped);
 reset();m.target=OTA_TARGET_DSP;launch(&m);run_job();assert(worker_deletes==1&&restores==1&&quiet_on==1&&quiet_off==1);
 reset();m.target=OTA_TARGET_BMS;launch(&m);run_job();assert(worker_deletes==1&&restores==1&&quiet_on==quiet_off);
 reset();admission_result=OTA_RESULT_DUPLICATE;assert(ota_service_start_remote(&m)==OTA_RESULT_DUPLICATE&&!notifications&&!auth_calls&&!reserve_calls);
 reset();admission_result=OTA_RESULT_TASK_REPLAY_BLOCKED;assert(ota_service_start_remote(&m)==OTA_RESULT_TASK_REPLAY_BLOCKED&&!notifications&&!auth_calls&&!reserve_calls);
 reset();g_remote_coordinator=NULL;assert(ota_service_start_remote(&m)==OTA_RESULT_TASK_CREATE_FAILED&&!notifications&&!g_remote_cleanup_busy&&!active);
 puts("PASS remote OTA: startup static coordinator, largest=1000/2000, full 12KB worker, repeated jobs/failures, cleanup exclusion, replay, cancellation and reboot");
}
'''
        for token, value in {
            "DEFINES": defines,
            "STORAGE": "\n".join(re.findall(r"^static (?:StackType_t g_remote_coordinator_stack\[.*|StaticTask_t g_remote_coordinator_tcb);?$", source, re.M)),
            "INIT": function(source, "start_remote_coordinator"),
            "CREATE": function(source, "create_ota_task_with_retry"),
            "COORDINATOR": function(source, "remote_coordinator"),
            "START": function(source, "ota_service_start_remote"),
            "QUIET_FUNCTIONS": function(source, "ota_quiet_begin") + '\n' + function(source, "ota_quiet_end"),
        }.items():
            harness = harness.replace("\n" + token + "\n", "\n" + value + "\n")
        with tempfile.TemporaryDirectory(dir=ROOT / "tmp", prefix="ota_remote_memory_") as folder:
            c = Path(folder) / "test.c"
            exe = Path(folder) / "test.exe"
            c.write_text(harness, encoding="utf-8")
            env = os.environ.copy()
            env["PATH"] = str(GCC.parent) + os.pathsep + env.get("PATH", "")
            env["TMP"] = env["TEMP"] = folder
            subprocess.run([str(GCC), "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(ROOT / "main/ota"), str(c), "-o", str(exe)],
                           check=True, env=env, timeout=30)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10, env=env)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout.strip())


if __name__ == "__main__":
    unittest.main()
