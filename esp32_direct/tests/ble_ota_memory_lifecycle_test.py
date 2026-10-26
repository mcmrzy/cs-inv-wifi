"""Run the firmware's actual BLE lifecycle functions with fake IDF boundaries.

These tests verify ordering, recovery and producer exclusion, not radio behavior
or reclaimed heap on a physical ESP32-C3.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GCC = os.environ.get("HOST_GCC") or shutil.which("gcc") or str(Path(
    r"C:\Users\29538\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe"))


def function(source, name):
    import re
    match = re.search(r"^(?:static\s+)?(?:bool|void|int)\s+" + name + r"\([^;]*?\)\s*\{", source, re.M)
    if not match:
        raise AssertionError("Missing BLE lifecycle function: " + name)
    start = match.start()
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class BleMemoryLifecycleTests(unittest.TestCase):
    def compile_run(self, source):
        with tempfile.TemporaryDirectory(prefix="ble_lifecycle_", dir=ROOT / "tmp") as folder:
            path = Path(folder)
            cfile, exe = path / "test.c", path / "test.exe"
            cfile.write_text(source, encoding="utf-8")
            env = os.environ.copy()
            env["PATH"] = str(Path(GCC).parent) + os.pathsep + env.get("PATH", "")
            env["TMP"] = env["TEMP"] = folder
            subprocess.run([GCC, "-std=c11", "-Wall", "-Wextra", "-Werror", str(cfile), "-o", str(exe)],
                           check=True, timeout=30, env=env)
            result = subprocess.run([str(exe)], text=True, capture_output=True, timeout=10, env=env)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout.strip())

    def lifecycle_harness(self, program):
        source = (ROOT / "components/ble_prov/ble_prov.c").read_text(encoding="utf-8")
        names = ("ble_prov_host_acquire", "ble_prov_host_release", "reset_host_handles",
                 "ble_prov_start", "ble_prov_stop", "ble_prov_resume",
                 "ble_prov_suspend_for_ota", "ble_prov_restore_after_ota", "ble_prov_notify_status",
                 "ble_prov_is_connected")
        helpers = ("lifecycle_acquire", "lifecycle_release", "mqtt_barrier_ack",
                   "mqtt_host_barrier", "mqtt_barrier_cleanup", "start_host_locked",
                   "suspend_owned", "restore_owned", "host_start_error",
                   "start_advertising", "nimble_host_reset", "nimble_host_sync")
        functions = "\n".join(function(source, name) for name in helpers
                              if name + "(" in source)
        functions += "\n" + "\n".join(function(source, name) for name in names)
        have_owners = "bool ble_prov_suspend_for_mqtt(" in source
        if have_owners:
            functions += "\n" + function(source, "ble_prov_suspend_for_mqtt")
            functions += "\n" + function(source, "ble_prov_restore_after_mqtt")
        have_readiness = "bool ble_prov_is_ready(" in source
        if have_readiness:
            functions += "\n" + function(source, "ble_prov_is_ready")
            functions += "\n" + function(source, "ble_prov_start_failed")
        have_recovery = "bool ble_prov_recover_start_failure(" in source
        if have_recovery:
            functions += "\n" + function(source, "ble_prov_recover_start_failure")
        have_suspended = "bool ble_prov_is_suspended(" in source
        if have_suspended:
            functions += "\n" + function(source, "ble_prov_is_suspended")
        declarations = source[source.index("static BleProvState"):source.index("/* 服务 UUID")]
        self.assertNotIn("esp_bt_controller_mem_release", functions)
        self.assertNotIn("esp_bt_mem_release", functions)
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
typedef enum { BLE_PROV_IDLE, BLE_PROV_RUNNING, BLE_PROV_DONE, BLE_PROV_ERROR } BleProvState;
typedef void (*BleProvCallback)(const char *, const char *);
typedef void (*BleProvConnCallback)(uint16_t);
typedef void (*BlePreHostStartCb)(void);
typedef void (*BleHostResetCb)(void);
typedef int (*BleGapEventForwardCb)(void *, void *);
typedef int esp_err_t;
typedef struct { int held; bool binary; } StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
static StaticSemaphore_t mutex_storage[4];
static int mutex_count, locked, init_calls, stop_calls, deinit_calls, host_delete_calls;
static int stack_live, host_live, stop_error, init_error, gatt_error, delete_order, reset_calls;
static int synced_calls, adv_calls, notify_calls, conn_callbacks, task_error, adv_stop_error, deinit_error;
static bool connect_during_adv_stop;
static size_t internal_free=100000, internal_largest=80000;
static int address_error, address_type_error, adv_fields_error, adv_rsp_error, adv_start_error;
static bool sync_in_task_create;
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
static size_t heap_caps_get_free_size(unsigned caps) { assert(caps==3); return internal_free; }
static size_t heap_caps_get_largest_free_block(unsigned caps) { assert(caps==3); return internal_largest; }
struct ble_npl_event { void (*fn)(struct ble_npl_event *); bool live; };
struct ble_npl_eventq { int unused; };
static struct ble_npl_eventq host_queue;
static struct ble_npl_event *queued_barrier;
static bool barrier_auto_ack=true, connect_at_barrier;
static unsigned barrier_posts, barrier_runs, barrier_timeouts, barrier_events_live;
static void process_host_queue(void);
#define pdMS_TO_TICKS(ms) (ms)
#define pdPASS 1
#define CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE 4096
#define configMAX_PRIORITIES 25
#define ESP_OK 0
#define BLE_HS_CONN_HANDLE_NONE 65535
#define BLE_HS_EALREADY 2
#define BLE_ERR_REM_USER_CONN_TERM 22
#define BLE_SM_IO_CAP_NO_IO 0
#define BLE_SM_PAIR_KEY_DIST_ENC 1
#define BLE_SM_PAIR_KEY_DIST_ID 2
#define pdTRUE 1
#define portMAX_DELAY 999
#define TAG "ble_prov"
#define ESP_LOGI(tag,...) do { (void)(tag); if(0) printf(__VA_ARGS__); } while(0)
#define ESP_LOGW(tag,...) ESP_LOGI(tag,__VA_ARGS__)
#define ESP_LOGE(tag,...) ESP_LOGI(tag,__VA_ARGS__)
static SemaphoreHandle_t xSemaphoreCreateMutex(void) { assert(mutex_count<4); return &mutex_storage[mutex_count++]; }
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *m) { return m; }
static SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *m) { m->binary=true; return m; }
static int xSemaphoreTake(SemaphoreHandle_t m, int timeout) {
    assert(m);
    if(m->binary) {
        if(timeout) {
            assert(timeout==1000 && locked==1);
            if(barrier_auto_ack) process_host_queue();
        }
        if(!m->held) { if(timeout) barrier_timeouts++; return 0; }
        m->held=0; return pdTRUE;
    }
    assert(!m->held); m->held=1; locked++; return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t m) {
    assert(m);
    if(m->binary) { assert(!m->held); m->held=1; return; }
    assert(m->held); m->held=0; locked--;
}
static struct ble_npl_eventq *nimble_port_get_dflt_eventq(void) { return &host_queue; }
static void ble_npl_event_init(struct ble_npl_event *ev, void (*fn)(struct ble_npl_event *), void *arg) {
    (void)arg; assert(stack_live && !ev->live && locked==1);
    ev->live=true; ev->fn=fn; barrier_events_live++;
}
static void ble_npl_event_deinit(struct ble_npl_event *ev) {
    assert(stack_live && !host_live && ev->live && queued_barrier!=ev && locked==1);
    ev->live=false; ev->fn=NULL; barrier_events_live--;
}
static void ble_npl_eventq_put(struct ble_npl_eventq *q, struct ble_npl_event *ev) {
    assert(q==&host_queue && host_live && ev->live && !queued_barrier && locked==1);
    queued_barrier=ev; barrier_posts++;
}
static void ble_npl_eventq_remove(struct ble_npl_eventq *q, struct ble_npl_event *ev) {
    assert(q==&host_queue && !host_live && locked==1);
    if(queued_barrier==ev) queued_barrier=NULL;
}
static size_t strlcpy(char *dst, const char *src, size_t n) {
    assert(dst != src); size_t len=strlen(src); if(n) { size_t copy=len<n-1?len:n-1; memcpy(dst,src,copy); dst[copy]=0; } return len;
}
static const char *esp_err_to_name(int error) { (void)error; return "failure"; }
static int nimble_port_init(void) { init_calls++; if(init_error) return init_error; assert(!stack_live); stack_live=1; return 0; }
static int nimble_port_stop(void) {
    stop_calls++; assert(stack_live && host_live && locked==1);
    assert(!ble_prov_host_acquire());
    return stop_error;
}
static void nimble_port_freertos_deinit(void) { assert(host_live); host_delete_calls++; host_live=0; delete_order=1; }
static void vTaskDelete(TaskHandle_t task) { assert(task && host_live && locked==1); host_delete_calls++; host_live=0; delete_order=1; }
static int nimble_port_deinit(void) { assert(stack_live && !host_live && locked==1 && !barrier_events_live && !queued_barrier); deinit_calls++; stack_live=0; return deinit_error; }
static void nimble_host_reset(int reason);
static void nimble_host_sync(void);
static void nimble_host_task(void *p) { (void)p; }
static struct { void (*reset_cb)(int); void (*sync_cb)(void); int sm_io_cap,sm_bonding,sm_mitm,sm_sc,sm_our_key_dist,sm_their_key_dist; } ble_hs_cfg;
static void nimble_port_freertos_init(void (*task)(void *)) { (void)task; assert(stack_live); host_live=!task_error; }
static int xTaskCreate(void (*task)(void *), const char *name, unsigned stack,
        void *arg, unsigned priority, TaskHandle_t *handle) {
    (void)task;(void)arg; assert(stack_live && locked==1);
    assert(!strcmp(name,"nimble_host") && stack==4096 && priority==21);
    if(task_error) { *handle=NULL; return 0; }
    host_live=1; *handle=(void *)1;
    if(sync_in_task_create) nimble_host_sync();
    return pdPASS;
}
static void ble_svc_gap_init(void) { assert(stack_live); }
static void ble_svc_gatt_init(void) { assert(stack_live); }
static int ble_svc_gap_device_name_set(const char *n) { assert(n && stack_live); return 0; }
static int s_gatt_svcs;
static int ble_gatts_count_cfg(void *p) { (void)p; assert(stack_live); return 0; }
static int ble_gatts_add_svcs(void *p) { (void)p; assert(stack_live); return gatt_error; }
static int ble_hs_synced(void) { assert(stack_live && host_live); synced_calls++; return 1; }
static int ble_gap_terminate(int conn,int reason) { (void)conn;(void)reason; assert(stack_live); return 0; }
static int ble_gap_adv_stop(void);
static void start_advertising(void);
#define BLE_HS_ADV_F_DISC_GEN 1
#define BLE_HS_ADV_F_BREDR_UNSUP 2
#define BLE_GAP_CONN_MODE_UND 1
#define BLE_GAP_DISC_MODE_GEN 1
#define BLE_HS_FOREVER 0
struct ble_hs_adv_fields { unsigned flags; uint8_t *name; size_t name_len;
    bool name_is_complete; const void *uuids128; unsigned num_uuids128; bool uuids128_is_complete; };
struct ble_gap_adv_params { unsigned conn_mode,disc_mode,itvl_min,itvl_max; };
static int s_svc_uuid;
static int gap_event_cb(void *event,void *arg) { (void)event;(void)arg; return 0; }
static int ble_hs_util_ensure_addr(int unused) { (void)unused; return address_error; }
static int ble_hs_id_infer_auto(int unused,uint8_t *type) { (void)unused; *type=0; return address_type_error; }
static int ble_gap_adv_set_fields(const struct ble_hs_adv_fields *fields) { (void)fields; return adv_fields_error; }
static int ble_gap_adv_rsp_set_fields(const struct ble_hs_adv_fields *fields) { (void)fields; return adv_rsp_error; }
static int ble_gap_adv_start(uint8_t type,void *peer,int timeout,struct ble_gap_adv_params *params,
        int (*callback)(void *,void *),void *arg) {
    (void)type;(void)peer;(void)timeout;(void)params;(void)callback;(void)arg;
    assert(stack_live && host_live); adv_calls++; return adv_start_error;
}
struct os_mbuf { int unused; };
static struct os_mbuf mbuf;
static struct os_mbuf *ble_hs_mbuf_from_flat(const void *p,size_t len) { (void)p;(void)len; assert(stack_live && host_live); return &mbuf; }
static int ble_gattc_notify_custom(int c,int h,struct os_mbuf *m) { (void)c;(void)h;(void)m; assert(stack_live && host_live); notify_calls++; return 0; }
static void reset_callback(void) { reset_calls++; }
static void connection_callback(uint16_t c) { assert(c==BLE_HS_CONN_HANDLE_NONE); conn_callbacks++; }
bool ble_prov_host_acquire(void);
void ble_prov_resume(void);
bool ble_prov_is_connected(void);
bool ble_prov_is_ready(void);
bool ble_prov_start_failed(void);
bool ble_prov_recover_start_failure(void);
bool ble_prov_is_suspended(void);
DECLARATIONS
static void reset_host_handles(void);
static void host_start_error(void);
static uint16_t s_sn_val_handle,s_fw_val_handle,s_mac_val_handle,s_ssid_val_handle,s_pass_val_handle;
static uint16_t s_ota_ctrl_handle,s_ota_data_handle,s_ota_status_handle;
static int ble_gap_adv_stop(void) {
    assert(stack_live);
    if(connect_during_adv_stop) s_conn_handle=9;
    return adv_stop_error;
}
static void process_host_queue(void) {
    if(!queued_barrier) return;
    assert(host_live && queued_barrier->live);
    if(connect_at_barrier) s_conn_handle=10;
    struct ble_npl_event *event=queued_barrier;
    queued_barrier=NULL; barrier_runs++;
    event->fn(event);
}
FUNCTIONS
int main(void) {
    (void)s_callback; (void)s_own_addr_type; (void)s_gap_forward_cb; (void)esp_err_to_name;
    (void)xSemaphoreCreateMutexStatic; (void)xTaskCreate; (void)vTaskDelete;
    (void)nimble_port_freertos_init; (void)nimble_port_freertos_deinit;
    (void)ble_prov_stop; (void)ble_prov_notify_status;
    (void)xSemaphoreCreateBinaryStatic; (void)nimble_port_get_dflt_eventq;
    (void)ble_npl_event_init; (void)ble_npl_event_deinit; (void)ble_npl_eventq_put;
    (void)ble_npl_eventq_remove; (void)barrier_posts; (void)barrier_runs;
    (void)heap_caps_get_free_size; (void)heap_caps_get_largest_free_block;
    (void)ble_prov_is_ready; (void)ble_prov_start_failed;
    (void)ble_prov_recover_start_failure;
    (void)ble_prov_is_suspended;
    s_host_reset_cb=reset_callback; s_conn_cb=connection_callback;
PROGRAM
}
'''.replace("DECLARATIONS", declarations).replace("FUNCTIONS", functions).replace("PROGRAM", program)
        if have_owners:
            harness = "#define HAVE_OWNER_API 1\n" + harness
        else:
            harness = "#define ble_prov_suspend_for_mqtt ble_prov_suspend_for_ota\n#define ble_prov_restore_after_mqtt ble_prov_restore_after_ota\n" + harness
        if not have_readiness:
            # Model the old public contract: accepted task was treated as ready.
            harness += "\nbool ble_prov_is_ready(void) { return s_host_started; }\nbool ble_prov_start_failed(void) { return false; }\n"
            harness = harness.replace("static void host_start_error(void);", "")
        if not have_recovery:
            harness += "\nbool ble_prov_recover_start_failure(void) { return false; }\n"
        if not have_suspended:
            harness += "\nbool ble_prov_is_suspended(void) { return false; }\n"
        harness = harness.replace("static int nimble_port_stop(void)", "bool ble_prov_host_acquire(void);\nstatic int nimble_port_stop(void)")
        harness = harness.replace("ble_gatts_count_cfg(s_gatt_svcs)", "ble_gatts_count_cfg(&s_gatt_svcs)")
        harness = harness.replace("ble_gatts_add_svcs(s_gatt_svcs)", "ble_gatts_add_svcs(&s_gatt_svcs)")
        return harness

    def test_recover_failed_host_with_no_advertising_then_start_fresh(self):
        self.compile_run(self.lifecycle_harness(r'''
    assert(ble_prov_start("SN123","0.6.19"));
    for(int i=0;i<5;i++) {
        assert(!ble_prov_recover_start_failure());
        assert(ble_prov_start("SN123","0.6.19"));
    }
    assert(init_calls==1 && !stop_calls && !ble_prov_is_ready());
    address_error=1; nimble_host_sync(); assert(ble_prov_start_failed());
    assert(!ble_prov_start("SN123","0.6.19"));
    adv_stop_error=BLE_HS_EALREADY;
    assert(ble_prov_recover_start_failure());
    assert(!host_live && !stack_live && stop_calls==1 && deinit_calls==1);
    assert(!ble_prov_start_failed() && !ble_prov_is_ready());
    address_error=0; adv_stop_error=0;
    assert(ble_prov_start("SN123","0.6.19")); nimble_host_sync();
    assert(ble_prov_is_ready() && init_calls==2 && !locked);
    puts("PASS: pending host never restarts; failed no-advertising host cleans up and starts fresh");
'''))

    def test_failure_recovery_preserves_peers_owners_and_timeout_retry(self):
        self.compile_run(self.lifecycle_harness(r'''
    assert(ble_prov_start("SN123","0.6.19"));
    adv_start_error=1; nimble_host_sync(); assert(ble_prov_start_failed());
    s_conn_handle=7; assert(!ble_prov_recover_start_failure()); assert(!stop_calls);
    s_conn_handle=BLE_HS_CONN_HANDLE_NONE; adv_stop_error=BLE_HS_EALREADY;
    connect_at_barrier=true;
    assert(!ble_prov_recover_start_failure()); assert(host_live && !stop_calls);
    connect_at_barrier=false; s_conn_handle=BLE_HS_CONN_HANDLE_NONE;
    barrier_auto_ack=false;
    assert(!ble_prov_recover_start_failure()); assert(host_live && !stop_calls);
    process_host_queue(); barrier_auto_ack=true; stop_error=1;
    assert(!ble_prov_recover_start_failure()); assert(host_live && !host_delete_calls);
    stop_error=0; assert(ble_prov_recover_start_failure());
    assert(!host_live && !barrier_events_live && !queued_barrier);
    adv_start_error=0; adv_stop_error=0;
    assert(ble_prov_start("SN123","0.6.19")); nimble_host_sync();
    assert(ble_prov_suspend_for_ota()); internal_free=55000;
    assert(!ble_prov_restore_after_ota()); assert(ble_prov_start_failed());
    assert(!ble_prov_recover_start_failure()); assert(!ble_prov_start("OTHER","other"));
    internal_free=100000; assert(ble_prov_restore_after_ota()); nimble_host_sync();
    assert(ble_prov_is_ready() && !locked);
    puts("PASS: recovery protects peers and owners, handles EALREADY and retries timeout/stop failure safely");
'''))

    def test_failure_recovery_without_host_and_deinit_failure_debt(self):
        self.compile_run(self.lifecycle_harness(r'''
    internal_free=55000; assert(!ble_prov_start("SN123","0.6.19"));
    assert(ble_prov_recover_start_failure()); assert(!init_calls && !stop_calls);
    internal_free=100000; task_error=1;
    assert(!ble_prov_start("SN123","0.6.19"));
    assert(ble_prov_recover_start_failure()); assert(!host_live);
    task_error=0; assert(ble_prov_start("SN123","0.6.19"));
    adv_start_error=1; nimble_host_sync(); deinit_error=1;
    assert(!ble_prov_recover_start_failure());
    assert(!host_live && ble_prov_start_failed());
    int before=deinit_calls;
    assert(!ble_prov_recover_start_failure()); assert(deinit_calls==before);
    assert(!ble_prov_start("OTHER","other") && !locked);
    puts("PASS: no-host failure clears for retry; partial deinit never reports cleanup success or reuses freed NPL");
'''))

    def test_internal_heap_admission_for_start_and_both_restore_paths(self):
        self.compile_run(self.lifecycle_harness(r'''
    internal_free=65535;
    assert(!ble_prov_start("SN123","0.6.19")); assert(!init_calls && !host_live);
    assert(!ble_prov_is_ready() && ble_prov_start_failed());
    internal_free=65536; internal_largest=32767;
    assert(!ble_prov_start("SN123","0.6.19")); assert(!init_calls);
    internal_largest=32768;
    assert(ble_prov_start("SN123","0.6.19")); assert(init_calls==1);
    assert(!ble_prov_is_ready() && !ble_prov_start_failed());
    nimble_host_sync(); assert(ble_prov_is_ready());
    assert(ble_prov_suspend_for_mqtt()); assert(!ble_prov_is_ready());
    internal_free=55000; assert(!ble_prov_restore_after_mqtt());
    assert(init_calls==1 && !host_live && ble_prov_start_failed());
    assert(!ble_prov_start("OTHER","other"));
    internal_free=65536; assert(ble_prov_restore_after_mqtt());
    assert(init_calls==2 && !ble_prov_is_ready() && !ble_prov_start_failed());
    nimble_host_sync(); assert(ble_prov_is_ready());
    assert(ble_prov_suspend_for_ota()); internal_largest=32767;
    assert(!ble_prov_restore_after_ota()); assert(init_calls==2 && !host_live);
    internal_largest=32768; assert(ble_prov_restore_after_ota());
    nimble_host_sync(); assert(ble_prov_is_ready() && !locked);
    puts("PASS: internal 8bit free/largest admission covers start, MQTT restore and OTA restore");
'''))

    def test_sync_address_and_advertising_failures_never_report_ready(self):
        self.compile_run(self.lifecycle_harness(r'''
    assert(ble_prov_start("SN123","0.6.19"));
    nimble_host_sync(); assert(ble_prov_is_ready());
    int *errors[]={&address_error,&address_type_error,&adv_fields_error,&adv_rsp_error,&adv_start_error};
    for(unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);i++) {
        *errors[i]=1; nimble_host_sync();
        assert(!ble_prov_is_ready() && ble_prov_start_failed() && s_state==BLE_PROV_ERROR);
        *errors[i]=0; nimble_host_sync();
        assert(ble_prov_is_ready() && !ble_prov_start_failed());
    }
    nimble_host_reset(99); assert(!ble_prov_is_ready() && ble_prov_start_failed());
    nimble_host_sync(); assert(ble_prov_is_ready());
    ble_prov_stop(); assert(!ble_prov_is_ready());
    assert(ble_prov_start("SN123","0.6.19")); assert(ble_prov_is_ready());
    ble_prov_stop(); adv_start_error=1;
    assert(!ble_prov_start("SN123","0.6.19")); assert(ble_prov_start_failed());
    adv_start_error=0; nimble_host_sync(); assert(ble_prov_is_ready());
    assert(ble_prov_suspend_for_ota()); assert(!ble_prov_is_ready());
    assert(ble_prov_restore_after_ota()); assert(!ble_prov_is_ready() && !ble_prov_start_failed());
    /* Repeated start while sync is pending cannot claim advertising is ready. */
    assert(ble_prov_start("SN123","0.6.19")); assert(!ble_prov_is_ready());
    nimble_host_sync(); assert(ble_prov_is_ready());
    assert(ble_prov_suspend_for_ota()); sync_in_task_create=true;
    assert(ble_prov_restore_after_ota()); assert(ble_prov_is_ready());
    assert(!locked);
    puts("PASS: accepted/pending, actual sync readiness, startup errors, reset, resume and fast sync");
'''))

    def test_provisioning_stop_free_restore_retry_and_no_host_access_while_suspended(self):
        self.compile_run(self.lifecycle_harness(r'''
    assert(ble_prov_suspend_for_ota()); assert(ble_prov_restore_after_ota()); assert(init_calls==0);
    assert(ble_prov_start("SN123","0.6.19")); assert(init_calls==1);
    s_conn_handle=7; s_status_val_handle=23; s_ota_data_handle=42;
    assert(ble_prov_host_acquire()); ble_prov_host_release();
    assert(!ble_prov_suspend_for_ota()); assert(host_live && !stop_calls);
    assert(s_ota_data_handle==42 && s_conn_handle==7);
    s_conn_handle=BLE_HS_CONN_HANDLE_NONE;
    assert(ble_prov_suspend_for_ota()); assert(!stack_live && !host_live);
    assert(host_delete_calls==1 && deinit_calls==1 && reset_calls>=1);
    assert(s_ota_data_handle==0 && s_conn_handle==BLE_HS_CONN_HANDLE_NONE);
    int sync_before=synced_calls; ble_prov_notify_status("failed"); ble_prov_resume(); ble_prov_stop();
    assert(!ble_prov_host_acquire()); assert(synced_calls==sync_before && notify_calls==0 && adv_calls==0);
    assert(ble_prov_suspend_for_ota()); assert(stop_calls==1);
    init_error=1; assert(!ble_prov_restore_after_ota()); assert(!stack_live);
    init_error=0; assert(ble_prov_restore_after_ota());
    assert(!strcmp(s_device_sn,"SN123") && !strcmp(s_fw_version,"0.6.19"));
    assert(ble_prov_restore_after_ota()); assert(init_calls==3);
    stop_error=1; assert(!ble_prov_suspend_for_ota()); assert(stack_live && host_live && deinit_calls==1);
    stop_error=0; assert(ble_prov_restore_after_ota()); assert(ble_prov_host_acquire()); ble_prov_host_release();
    assert(ble_prov_suspend_for_ota()); gatt_error=1; assert(!ble_prov_restore_after_ota()); assert(!stack_live);
    gatt_error=0; assert(ble_prov_restore_after_ota());
    assert(ble_prov_suspend_for_ota()); assert(ble_prov_restore_after_ota());
    ble_prov_stop(); assert(!s_advertising_enabled && s_state==BLE_PROV_IDLE);
    assert(ble_prov_suspend_for_ota()); assert(ble_prov_restore_after_ota());
    int before_resume=adv_calls;
    ble_prov_resume(); assert(s_advertising_enabled && adv_calls==before_resume+1);
    assert(!locked && s_conn_cb==connection_callback && s_host_reset_cb==reset_callback);
    puts("PASS: BLE reversible stop/deinit/reinit, retries, identity and host exclusion");
'''))

    def test_owners_interleave_without_early_restore_or_start_bypass(self):
        self.compile_run(self.lifecycle_harness(r'''
    assert(ble_prov_suspend_for_mqtt());
    assert(!ble_prov_start("SN123","0.6.19"));
    assert(ble_prov_restore_after_ota()); assert(!stack_live);
    assert(ble_prov_restore_after_mqtt());
    assert(ble_prov_start("SN123","0.6.19"));
    assert(ble_prov_suspend_for_mqtt()); assert(!host_live);
    assert(ble_prov_suspend_for_ota());
    assert(ble_prov_suspend_for_mqtt());
    assert(ble_prov_restore_after_mqtt()); assert(!stack_live);
    assert(ble_prov_restore_after_mqtt()); assert(!stack_live);
    assert(!ble_prov_start("OTHER","other"));
    assert(ble_prov_restore_after_ota()); assert(stack_live && host_live);
    assert(!strcmp(s_device_sn,"SN123"));
    assert(ble_prov_suspend_for_ota()); assert(ble_prov_suspend_for_mqtt());
    assert(ble_prov_restore_after_ota()); assert(!stack_live);
    init_error=1; assert(!ble_prov_restore_after_mqtt());
    assert(!ble_prov_start("OTHER","other"));
    assert(ble_prov_restore_after_ota()); assert(!stack_live);
    init_error=0; assert(ble_prov_restore_after_mqtt()); assert(host_live);
    assert(!locked);
    puts("PASS: independent OTA/MQTT owners, idempotence, restore retries and start exclusion");
'''))

    def test_host_task_creation_failure_rolls_back_and_can_retry(self):
        self.compile_run(self.lifecycle_harness(r'''
    task_error=1;
    assert(!ble_prov_start("SN123","0.6.19"));
    assert(!stack_live && !host_live && !s_host_initialized && !s_host_started);
    assert(deinit_calls==1 && !locked);
    task_error=0; assert(ble_prov_start("SN123","0.6.19"));
    assert(ble_prov_suspend_for_mqtt());
    task_error=1; assert(!ble_prov_restore_after_mqtt());
    assert(!stack_live && !host_live && !s_host_started);
    assert(!ble_prov_start("OTHER","other"));
    assert(ble_prov_restore_after_ota()); assert(!host_live);
    task_error=0; assert(ble_prov_restore_after_mqtt()); assert(host_live);
    assert(ble_prov_suspend_for_ota()); assert(ble_prov_restore_after_ota());
    assert(!locked);
    puts("PASS: failed host creation deinitializes and retains the restoring owner until retry");
'''))

    def test_mqtt_idle_pause_rejects_connections_and_stop_errors_without_ownership(self):
        self.compile_run(self.lifecycle_harness(r'''
    assert(ble_prov_start("SN123","0.6.19"));
    s_conn_handle=7;
    assert(!ble_prov_suspend_for_mqtt()); assert(host_live && !stop_calls);
    assert(ble_prov_host_acquire()); ble_prov_host_release();
    s_conn_handle=BLE_HS_CONN_HANDLE_NONE; connect_during_adv_stop=true;
    assert(!ble_prov_suspend_for_mqtt()); assert(host_live && !stop_calls);
    assert(ble_prov_restore_after_mqtt()); assert(host_live);
    connect_during_adv_stop=false; s_conn_handle=BLE_HS_CONN_HANDLE_NONE;
    adv_stop_error=1;
    assert(!ble_prov_suspend_for_mqtt()); assert(host_live && !stop_calls);
    adv_stop_error=0; assert(ble_prov_restore_after_mqtt());
    stop_error=1; assert(!ble_prov_suspend_for_mqtt()); assert(host_live);
    stop_error=0; assert(ble_prov_restore_after_mqtt());
    assert(ble_prov_host_acquire()); ble_prov_host_release();
    assert(ble_prov_suspend_for_ota());
    assert(ble_prov_restore_after_mqtt()); assert(!host_live);
    assert(ble_prov_restore_after_ota()); assert(host_live);
    assert(ble_prov_suspend_for_mqtt()); assert(ble_prov_restore_after_mqtt());
    assert(!locked);
    puts("PASS: MQTT never stops observed active/new BLE connections, failed pauses recover without stealing ownership");
'''))

    def test_failed_teardown_cannot_be_claimed_by_another_owner(self):
        self.compile_run(self.lifecycle_harness(r'''
    assert(ble_prov_start("SN123","0.6.19"));
    deinit_error=1; assert(!ble_prov_suspend_for_mqtt());
    assert(!ble_prov_suspend_for_ota());
    assert(!ble_prov_start("OTHER","other"));
    deinit_error=0; assert(ble_prov_restore_after_mqtt()); assert(host_live);
    assert(ble_prov_suspend_for_ota()); assert(ble_prov_restore_after_ota());
    assert(!locked);
    puts("PASS: failed deinit remains recovery debt, never successful pause ownership");
'''))

    def test_mqtt_barrier_observes_connection_queued_before_advstop_ack(self):
        self.compile_run(self.lifecycle_harness(r'''
    assert(ble_prov_start("SN123","0.6.19"));
    connect_at_barrier=true;
    assert(!ble_prov_suspend_for_mqtt());
    assert(barrier_posts==1 && barrier_runs==1 && host_live && !stop_calls);
    assert(s_conn_handle==10 && !s_ota_suspended);
    assert(ble_prov_restore_after_mqtt()); assert(host_live);
    connect_at_barrier=false; s_conn_handle=BLE_HS_CONN_HANDLE_NONE;
    assert(ble_prov_suspend_for_mqtt()); assert(!host_live);
    assert(ble_prov_restore_after_mqtt());
    assert(ble_prov_suspend_for_mqtt()); assert(ble_prov_restore_after_mqtt());
    assert(barrier_posts==3 && barrier_runs==3 && !locked);
    puts("PASS: queued connection becomes visible before idle stop; barrier survives full host restart");
'''))

    def test_mqtt_barrier_timeout_late_ack_and_fresh_ack_required(self):
        self.compile_run(self.lifecycle_harness(r'''
    assert(ble_prov_start("SN123","0.6.19"));
    barrier_auto_ack=false;
    assert(!ble_prov_suspend_for_mqtt());
    assert(host_live && !stop_calls && barrier_posts==1 && barrier_timeouts==1);
    assert(!s_ota_suspended && s_advertising_enabled && adv_calls==1);
    assert(!ble_prov_suspend_for_mqtt());
    assert(barrier_posts==1 && !stop_calls); /* Pending event must not be reposted. */
    process_host_queue(); assert(barrier_runs==1); /* ACK arrives after timeout. */
    assert(!ble_prov_suspend_for_mqtt());
    assert(barrier_posts==2 && barrier_timeouts==2 && !stop_calls);
    /* The old ACK did not admit a new attempt; only its new ACK can do so. */
    process_host_queue(); barrier_auto_ack=true;
    assert(ble_prov_suspend_for_mqtt());
    assert(barrier_posts==3 && barrier_runs==3 && stop_calls==1);
    assert(ble_prov_restore_after_mqtt());
    barrier_auto_ack=false; assert(!ble_prov_suspend_for_mqtt());
    assert(!ble_prov_suspend_for_ota()); /* No forced teardown of pending barrier. */
    assert(host_live && queued_barrier && barrier_events_live && barrier_posts==4);
    process_host_queue();
    assert(!ble_prov_suspend_for_ota()); /* Old ACK cannot admit a new OTA pause. */
    assert(host_live && barrier_posts==5);
    process_host_queue(); barrier_auto_ack=true;
    assert(ble_prov_suspend_for_ota()); /* Fresh ACK permits safe stop/delete. */
    assert(!queued_barrier && !barrier_events_live);
    assert(ble_prov_restore_after_ota());
    barrier_auto_ack=true; assert(ble_prov_suspend_for_mqtt());
    assert(ble_prov_restore_after_mqtt()); assert(!locked);
    puts("PASS: bounded ACK wait, no timeout repost or stale ACK admission; teardown safely resets pending event");
'''))

    def test_ota_idle_pause_protects_active_and_racing_phone(self):
        self.compile_run(self.lifecycle_harness(r'''
    assert(ble_prov_start("SN123","0.6.19"));
    s_conn_handle=7;
    assert(!ble_prov_suspend_for_ota()); assert(host_live && !stop_calls && !barrier_posts);
    assert(!ble_prov_is_suspended());
    s_conn_handle=BLE_HS_CONN_HANDLE_NONE; connect_during_adv_stop=true;
    assert(!ble_prov_suspend_for_ota()); assert(host_live && !stop_calls);
    assert(!ble_prov_is_suspended());
    connect_during_adv_stop=false; s_conn_handle=BLE_HS_CONN_HANDLE_NONE;
    connect_at_barrier=true;
    assert(!ble_prov_suspend_for_ota()); assert(host_live && !stop_calls && s_conn_handle==10);
    assert(!ble_prov_is_suspended());
    connect_at_barrier=false; s_conn_handle=BLE_HS_CONN_HANDLE_NONE;
    adv_stop_error=BLE_HS_EALREADY;
    assert(!ble_prov_suspend_for_ota()); assert(host_live && !stop_calls);
    adv_stop_error=0;
    assert(ble_prov_suspend_for_mqtt()); assert(ble_prov_is_suspended());
    assert(ble_prov_restore_after_ota()); assert(!host_live && ble_prov_is_suspended());
    assert(ble_prov_restore_after_mqtt()); assert(host_live && !ble_prov_is_suspended());
    assert(!locked);
    puts("PASS: OTA never terminates active/advstop/barrier-racing phone, failed OTA acquires no owner");
'''))

    def test_suspended_snapshot_is_producer_gate_not_reclaimed_memory(self):
        self.compile_run(self.lifecycle_harness(r'''
    assert(!ble_prov_is_suspended());
    assert(ble_prov_suspend_for_ota()); assert(ble_prov_is_suspended());
    assert(ble_prov_restore_after_ota()); assert(!ble_prov_is_suspended());
    assert(ble_prov_start("SN123","0.6.19")); stop_error=1;
    assert(!ble_prov_suspend_for_ota());
    assert(ble_prov_is_suspended() && stack_live && host_live); /* Memory not freed. */
    stop_error=0; assert(ble_prov_restore_after_ota()); assert(!ble_prov_is_suspended());
    assert(ble_prov_suspend_for_ota()); assert(ble_prov_is_suspended() && !stack_live);
    internal_free=55000; assert(!ble_prov_restore_after_ota());
    assert(ble_prov_is_suspended());
    internal_free=100000; assert(ble_prov_restore_after_ota());
    assert(!ble_prov_is_suspended() && !locked);
    puts("PASS: suspended is an atomic producer gate snapshot across no-host, failed stop and restore retry");
'''))

    def test_control_services_reregister_without_stale_handles_or_subscriptions(self):
        source = (ROOT / "components/ble_ct/ble_ct.c").read_text(encoding="utf-8")
        reset = function(source, "ble_ct_reset_host")
        register = function(source, "ble_ct_register_services")
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define BLE_HS_CONN_HANDLE_NONE 65535
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
static bool s_services_registered,s_telemetry_subscribed,s_result_subscribed,s_framing_enabled;
static uint16_t s_conn_handle,s_telemetry_handle,s_cmd_handle,s_result_handle,s_info_handle;
static char s_session_id[33];
static int s_ct_gatt_svcs,registration_count;
static unsigned command_resets;
static void ble_ct_cmd_reset_connection(void) { command_resets++; }
static void ble_ct_telemetry_reset_connection(void) {}
static int ble_gatts_count_cfg(int svc) { (void)svc; return 0; }
static int ble_gatts_add_svcs(int svc) { (void)svc; registration_count++; return 0; }
RESET
REGISTER
int main(void) {
    ble_ct_register_services(); ble_ct_register_services(); assert(registration_count==1);
    s_conn_handle=7; s_telemetry_handle=11; s_cmd_handle=12; s_result_handle=13; s_info_handle=14;
    s_telemetry_subscribed=s_result_subscribed=s_framing_enabled=true; s_session_id[0]='x';
    ble_ct_reset_host(); assert(s_conn_handle==BLE_HS_CONN_HANDLE_NONE && !s_session_id[0]);
    assert(command_resets==1);
    assert(!s_telemetry_handle && !s_cmd_handle && !s_result_handle && !s_info_handle);
    assert(!s_telemetry_subscribed && !s_result_subscribed && !s_framing_enabled);
    ble_ct_register_services(); assert(registration_count==2);
    puts("PASS: fresh CT services, handles and subscriptions after BLE host restoration");
}
'''.replace("RESET", reset).replace("REGISTER", register)
        self.compile_run(harness)

    def test_background_notifications_hold_gate_and_skip_closed_host(self):
        telemetry = (ROOT / "components/ble_ct/ble_ct_telemetry.c").read_text(encoding="utf-8")
        command = (ROOT / "components/ble_ct/ble_ct_cmd.c").read_text(encoding="utf-8")
        ota = (ROOT / "main/ota/ble_ota.c").read_text(encoding="utf-8")
        functions = "\n".join(function(telemetry, name) for name in
                              ("ble_ct_telemetry_set_snapshot", "ble_ct_telemetry_should_push", "send_frame", "ble_ct_telemetry_notify"))
        functions += "\n" + function(command, "ble_ct_cmd_notify_result")
        functions += "\n" + function(ota, "notify_status")
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "CODEC"
#define ESP_LOGW(...) ((void)0)
#define SNAP_MAX BLE_CT_FRAME_MAX_MESSAGE_BYTES
#define RESULT_CAPACITY 2048
#define LEGACY_MAX_FRAMES 8u
#define FRAME_CTRL_FIRST 0x80
#define FRAME_CTRL_LAST 0x40
#define FRAME_CTRL_SINGLE 0xC0
#define BLE_HS_CONN_HANDLE_NONE 65535
static char s_snapshot[SNAP_MAX+1u];
static uint16_t s_snapshot_len;
typedef int portMUX_TYPE;
static portMUX_TYPE s_snapshot_lock;
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
static bool gate_open,held,subscribed=true;
static unsigned allocated,notified,acquired,released;
static uint16_t connection=7;
static uint16_t s_conn=7;
static char s_status[]="{}";
static bool ble_prov_host_acquire(void) { if(!gate_open)return false; assert(!held); held=true; acquired++; return true; }
static void ble_prov_host_release(void) { assert(held); held=false; released++; }
static bool ble_ct_is_telemetry_subscribed(void) { return subscribed; }
static bool ble_ct_is_result_subscribed(void) { return subscribed; }
static bool ble_ct_framing_enabled(void) { return true; }
static uint16_t ble_att_mtu(uint16_t c) { (void)c; assert(held); return 512; }
static uint16_t ble_ct_get_conn_handle(void) { return connection; }
static uint16_t ble_ct_get_telemetry_handle(void) { return 11; }
static uint16_t ble_ct_get_result_handle(void) { return 12; }
static uint16_t ble_prov_get_ota_status_handle(void) { return 13; }
struct os_mbuf { int unused; };
static struct os_mbuf mbuf;
static struct os_mbuf *ble_hs_mbuf_from_flat(const void *p,unsigned len) { (void)p;(void)len; assert(held && gate_open); allocated++; return &mbuf; }
static void os_mbuf_free_chain(struct os_mbuf *p) { (void)p; assert(held); }
static void os_mbuf_concat(struct os_mbuf *p,struct os_mbuf *q) { (void)p;(void)q; assert(held); }
static int ble_gattc_notify_custom(int conn,int handle,struct os_mbuf *p) { (void)conn;(void)handle;(void)p; assert(held && gate_open); notified++; return 0; }
FUNCTIONS
int main(void) {
    (void)ble_prov_host_acquire; (void)ble_prov_host_release;
    (void)os_mbuf_free_chain; (void)os_mbuf_concat;
    char long_json[5000]; memset(long_json,'x',sizeof(long_json));
    ble_ct_telemetry_notify("{}",2); ble_ct_cmd_notify_result("{}",2); notify_status(); assert(!allocated && !notified);
    gate_open=true;
    ble_ct_telemetry_notify("{}",2); ble_ct_telemetry_notify(long_json,600); ble_ct_cmd_notify_result("{}",2);
    assert(notified==4 && !held && acquired==released);
    notify_status(); assert(notified==5 && !held && acquired==released);
    unsigned count=notified;
    subscribed=false; ble_ct_telemetry_notify("{}",2); subscribed=true;
    ble_ct_telemetry_notify(long_json,5000);
    connection=BLE_HS_CONN_HANDLE_NONE; ble_ct_telemetry_notify("{}",2); ble_ct_cmd_notify_result("{}",2);
    s_conn=BLE_HS_CONN_HANDLE_NONE; notify_status();
    assert(notified==count && !held && acquired==released);
    puts("PASS: telemetry and command notifications drain before host deinit and skip the stopped host");
}
'''.replace("FUNCTIONS", functions).replace("CODEC", (ROOT / "components/ble_ct/ble_ct_frames.c").as_posix())
        self.compile_run(harness)


if __name__ == "__main__":
    unittest.main()
