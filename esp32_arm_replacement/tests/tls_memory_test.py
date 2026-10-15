"""Check TLS build profiles and execute the real boot diagnostic task on a host.

Only the ESP/FreeRTOS boundary is faked. These checks do not measure device heap
or substitute for MQTT reconnect and HTTPS OTA acceptance on hardware.
"""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GCC = Path(r"C:\Users\29538\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe")


def extract_function(source, name):
    start = source.index(name + "(")
    start = source.rfind("\n", 0, start) + 1
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class TlsMemoryTests(unittest.TestCase):
    def test_memory_log_reports_internal_byte_heap(self):
        source = (ROOT / "main/mqtt/mqtt_handler.c").read_text(encoding="utf-8")
        self.assertTrue("static void mqtt_log_tls_memory(" in source,
                        "TLS memory diagnostics are missing")
        helper = extract_function(source, "mqtt_log_tls_memory")
        harness = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#define MALLOC_CAP_INTERNAL 1U
#define MALLOC_CAP_8BIT 2U
typedef struct {
    size_t total_free_bytes, largest_free_block, minimum_free_bytes;
} multi_heap_info_t;
static const char *TAG = "MQTT";
static char captured[256];
static void heap_caps_get_info(multi_heap_info_t *info, unsigned caps) {
    assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    *info = (multi_heap_info_t){24000, 12000, 8000};
}
static void capture(const char *tag, const char *format, ...) {
    assert(!strcmp(tag, "MQTT"));
    va_list args; va_start(args, format);
    vsnprintf(captured, sizeof(captured), format, args); va_end(args);
}
#define ESP_LOGW capture
HELPER
int main(void) {
    mqtt_log_tls_memory("before_connect");
    assert(strstr(captured, "phase=before_connect"));
    assert(strstr(captured, "free=24000"));
    assert(strstr(captured, "largest=12000"));
    assert(strstr(captured, "min=8000"));
    puts("PASS: TLS memory diagnostics report free/largest/min from internal byte-addressable RAM");
}
'''.replace("HELPER", helper)
        self.compile_and_run(harness)

    def compile_and_run(self, harness):
        with tempfile.TemporaryDirectory(prefix="tls_memory_", dir=ROOT / "tmp") as folder:
            c_file = Path(folder) / "tls_memory_test.c"
            exe = Path(folder) / "tls_memory_test.exe"
            c_file.write_text(harness, encoding="utf-8")
            env = os.environ.copy()
            env["PATH"] = str(GCC.parent) + os.pathsep + env.get("PATH", "")
            env["TMP"] = env["TEMP"] = folder
            subprocess.run([str(GCC), "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(c_file), "-o", str(exe)], check=True, timeout=30, env=env)
            result = subprocess.run([str(exe)], capture_output=True, text=True,
                                    timeout=10, env=env)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout.strip())

    def test_all_build_profiles_enable_dynamic_tls_and_keep_https_record_size(self):
        for profile in ("sdkconfig", "sdkconfig.defaults", "sdkconfig.device",
                        "sdkconfig.production", "sdkconfig.factory"):
            with self.subTest(profile=profile):
                config = dict(line.split("=", 1) for line in
                              (ROOT / profile).read_text(encoding="utf-8").splitlines()
                              if line.startswith("CONFIG_"))
                self.assertEqual(config.get("CONFIG_MBEDTLS_DYNAMIC_BUFFER"), "y",
                                 "TLS buffers must not remain permanently allocated")
                self.assertEqual(config["CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN"], "16384")
                self.assertEqual(config["CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN"], "2048")

    def test_boot_diagnostics_release_stack_at_terminal_stages(self):
        source = (ROOT / "main/app/app_main.c").read_text(encoding="utf-8")
        enum_start = source.rfind("typedef enum", 0, source.index("BOOT_BUZZ"))
        enum_end = source.index("direct_boot_stage_t;", enum_start) + len("direct_boot_stage_t;")
        task = extract_function(source, "boot_diagnostic_task")
        harness = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
ENUM
static atomic_int s_boot_stage;
static jmp_buf stopped;
static unsigned delays, logs;
static int next_stage;
static char last_log[256];
static int capture_printf(const char *format, ...) {
    va_list args; va_start(args, format);
    int result = vsnprintf(last_log, sizeof(last_log), format, args);
    va_end(args); logs++; return result;
}
static int64_t esp_timer_get_time(void) { return 1000; }
static int esp_reset_reason(void) { return 0; }
static unsigned esp_get_free_heap_size(void) { return 25000; }
#define pdMS_TO_TICKS(ms) (ms)
static void vTaskDelete(void *handle) { assert(handle == NULL); longjmp(stopped, 1); }
static void vTaskDelay(unsigned ms) {
    assert(ms == 2000); delays++;
    if (next_stage >= 0) atomic_store(&s_boot_stage, next_stage);
    if (delays >= 4) longjmp(stopped, 2);
}
#define printf capture_printf
TASK
#undef printf
static void run_case(int initial, int next, int expected_stop,
                     unsigned expected_delays, const char *expected_stage) {
    delays = logs = 0; next_stage = next; last_log[0] = '\0';
    atomic_store(&s_boot_stage, initial);
    int reason = setjmp(stopped);
    if (!reason) boot_diagnostic_task(NULL);
    assert(reason == expected_stop);
    assert(delays == expected_delays);
    assert(logs == (expected_stop == 1 ? expected_delays + 1 : expected_delays));
    assert(strstr(last_log, expected_stage));
}
int main(void) {
    (void)vTaskDelete;
    run_case(BOOT_READY, -1, 1, 0, "stage=READY");
    run_case(BOOT_FAILED, -1, 1, 0, "stage=FAILED");
    run_case(BOOT_NETWORK, BOOT_READY, 1, 1, "stage=READY");
    run_case(BOOT_NETWORK, BOOT_FAILED, 1, 1, "stage=FAILED");
    run_case(BOOT_NETWORK, -1, 2, 4, "stage=NETWORK");
    puts("PASS: boot diagnostics release their stack after READY/FAILED and retain stalled-startup evidence");
    return 0;
}
'''.replace("ENUM", source[enum_start:enum_end]).replace("TASK", task)
        self.compile_and_run(harness)


if __name__ == "__main__":
    unittest.main()
