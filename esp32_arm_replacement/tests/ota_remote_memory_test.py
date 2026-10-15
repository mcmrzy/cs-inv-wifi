"""Execute the real remote OTA launcher/coordinator with a fragmented heap.

FreeRTOS and BLE are fake boundaries; hardware heap and TLS remain acceptance
items. The log's largest=7680 must admit the coordinator, not a smaller worker.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GCC = Path(r"C:\Users\29538\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe")


def function(source, name):
    start = source.index(name + "(")
    start = source.rfind("\n", 0, start) + 1
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class RemoteMemoryTests(unittest.TestCase):
    def test_fragmented_heap_and_recovery(self):
        source = (ROOT / "main/ota/ota_service.c").read_text(encoding="utf-8")
        defines = "\n".join(re.findall(r"^#define OTA_(?:TASK_STACK_SIZE|TASK_PRIORITY|ALLOC_ATTEMPTS|ALLOC_RETRY_MS|COORDINATOR_STACK_SIZE|COORDINATOR_PRIORITY)\s+.*$", source, re.M))
        coordinator = (function(source, "remote_coordinator")
                       if "static void remote_coordinator(" in source else "")
        harness = r'''
#include "ota_service.h"
#include <assert.h>
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
static void *g_lock=(void*)1;
static TaskHandle_t g_remote_task, g_remote_coordinator;
static bool g_remote_cleanup_busy, g_remote_reboot_pending;
static OtaManifest g_manifest;
static bool active, ble_stopped, suspend_ok, restore_ok, cancel, fail_worker;
static unsigned largest, failures, restores, worker_deletes, quiet_on, quiet_off;
static OtaResult last_failure;
static TaskFunction_t queued;
static unsigned heap_caps_get_free_size(unsigned c){(void)c;return 16000;}
static unsigned heap_caps_get_largest_free_block(unsigned c){(void)c;return largest;}
static void vTaskDelay(unsigned n){(void)n;}
static int xSemaphoreTake(void*l,unsigned n){assert(l);(void)n;return pdTRUE;}
static void xSemaphoreGive(void*l){assert(l);}
static int xTaskCreate(TaskFunction_t f,const char*n,unsigned stack,void*p,unsigned prio,TaskHandle_t*h){
 (void)p;(void)prio;
 if(stack>largest || (!strcmp(n,"ota_remote")&&fail_worker))return 0;
 if(!strcmp(n,"ota_remote")){assert(ble_stopped&&stack>=12288);if(h)*h=(void*)3;}
 else {assert(stack<=7680);queued=f;if(h)*h=(void*)2;}
 return pdPASS;
}
static void vTaskDelete(void*h){if(h==(void*)3){worker_deletes++;largest+=12288;}else assert(h==NULL);}
static unsigned ulTaskNotifyTake(int clear,unsigned wait){assert(clear&&wait==portMAX_DELAY&&g_remote_task);active=false;return 1;}
static bool ble_prov_suspend_for_ota(void){if(!suspend_ok)return false;ble_stopped=true;largest=50000;return true;}
static bool ble_prov_restore_after_ota(void){assert(!g_remote_task);if(ble_stopped)assert(largest>=12288);restores++;if(restore_ok)ble_stopped=false;return restore_ok;}
static void mqtt_publish_ota_trace(const char*a,const char*b){assert(a&&b);}
static void ota_quiet_begin(void){quiet_on++;}
static void ota_quiet_end(void){quiet_off++;}
static bool cancel_requested(void){return cancel;}
static OtaResult validate_manifest_cheap(const OtaManifest*m){assert(m);return OTA_RESULT_OK;}
static OtaResult check_available(const OtaManifest*m){(void)m;return active||g_remote_cleanup_busy?OTA_RESULT_BUSY:OTA_RESULT_OK;}
static OtaResult authenticate_manifest(const OtaManifest*m){(void)m;return OTA_RESULT_OK;}
static OtaResult reserve_manifest(const OtaManifest*m){g_manifest=*m;active=true;return OTA_RESULT_OK;}
static void arm_ota_workspace_release(void){}
static void finish_failure(OtaResult r,const char*m){assert(m);failures++;last_failure=r;active=false;}
static void remote_worker(void*p){(void)p;}
DEFINES
CREATE
COORDINATOR
START
static void reset(void){
 active=ble_stopped=cancel=fail_worker=g_remote_cleanup_busy=g_remote_reboot_pending=false;
 suspend_ok=restore_ok=true;largest=7680;failures=restores=worker_deletes=quiet_on=quiet_off=0;
 g_remote_task=g_remote_coordinator=NULL;queued=NULL;
}
static void launch(OtaManifest*m){assert(ota_service_start_remote(m)==OTA_RESULT_OK&&queued);}
int main(void){
 OtaManifest m={.target=OTA_TARGET_ESP};
 reset();launch(&m);assert(g_remote_cleanup_busy);assert(ota_service_start_remote(&m)==OTA_RESULT_BUSY);
 queued(NULL);assert(worker_deletes==1&&restores==1&&!ble_stopped&&!g_remote_cleanup_busy&&quiet_on==quiet_off);
 reset();fail_worker=true;launch(&m);queued(NULL);assert(failures==1&&last_failure==OTA_RESULT_TASK_CREATE_FAILED&&restores==1&&!g_remote_cleanup_busy&&quiet_on==quiet_off);
 reset();suspend_ok=false;launch(&m);queued(NULL);assert(failures==1&&!worker_deletes&&!g_remote_cleanup_busy&&quiet_on==quiet_off);
 reset();cancel=true;launch(&m);queued(NULL);assert(failures==1&&last_failure==OTA_RESULT_OK&&!worker_deletes&&quiet_on==quiet_off);
 reset();largest=2000;assert(ota_service_start_remote(&m)==OTA_RESULT_TASK_CREATE_FAILED&&!g_remote_cleanup_busy&&!ble_stopped);
 reset();g_remote_reboot_pending=true;launch(&m);g_remote_reboot_pending=true;queued(NULL);assert(worker_deletes==1&&restores==0&&ble_stopped);
 reset();m.target=OTA_TARGET_DSP;launch(&m);queued(NULL);assert(worker_deletes==1&&restores==1&&!quiet_on&&!quiet_off);
 puts("PASS remote OTA: 7680-byte fragmented heap, full 12KB worker, recovery, cancellation, duplicate exclusion, reboot and DSP cleanup");
}
'''
        for token, value in {
            "DEFINES": defines,
            "CREATE": function(source, "create_ota_task_with_retry"),
            "COORDINATOR": coordinator,
            "START": function(source, "ota_service_start_remote"),
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
