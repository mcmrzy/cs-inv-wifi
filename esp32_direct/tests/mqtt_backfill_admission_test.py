"""Real cache-worker admission: OTA/local pause must close the task-create race."""
from pathlib import Path
import unittest
from mqtt_connection_window_test import function
import mqtt_local_mode_test as local_mode

ROOT=Path(__file__).resolve().parents[1]


class BackfillTests(unittest.TestCase):
    def test_pause_drains_and_prevents_late_worker_allocation(self):
        source=(ROOT/'main/mqtt/mqtt_handler.c').read_text(encoding='utf-8')
        code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define OFFLINE_FLUSH_TASK_STACK 4096
#define pdPASS 1
static atomic_bool g_local_paused,g_cache_flush_paused,g_cache_flush_running,g_mqtt_connected=true,g_flush_pending;
static void *g_mqtt_client=(void*)1;
static const char *TAG="test";
static bool pause_in_count,fail_create;
static unsigned created,counted;
bool mqtt_set_cache_flush_paused(bool);
static int offline_cache_count(void){counted++;if(pause_in_count)assert(mqtt_set_cache_flush_paused(true));return 10;}
static void offline_cache_flush_task(void*p){(void)p;}
static int xTaskCreate(void(*f)(void*),const char*n,unsigned stack,void*p,unsigned priority,void*h){
 (void)f;(void)n;(void)p;(void)priority;(void)h;
 assert(!atomic_load(&g_cache_flush_paused)&&!atomic_load(&g_local_paused));
 assert(atomic_load(&g_cache_flush_running)&&stack==4096);created++;return fail_create?0:1;
}
FUNCTIONS
int main(void){
 assert(mqtt_set_cache_flush_paused(true));mqtt_flush_offline_cache();assert(!created&&!counted);
 assert(mqtt_set_cache_flush_paused(false));pause_in_count=true;
 mqtt_flush_offline_cache();assert(!created&&!atomic_load(&g_cache_flush_running));
 pause_in_count=false;mqtt_set_cache_flush_paused(false);mqtt_flush_offline_cache();assert(created==1);
 assert(!mqtt_set_cache_flush_paused(true));mqtt_flush_offline_cache();assert(created==1);
 atomic_store(&g_cache_flush_running,false);assert(mqtt_set_cache_flush_paused(true));
 mqtt_set_cache_flush_paused(false);fail_create=true;mqtt_flush_offline_cache();assert(created==2&&!atomic_load(&g_cache_flush_running));
 puts("PASS real backfill admission: pause, drain, late pause race, retry after task-allocation failure");
}
'''.replace('FUNCTIONS','\n'.join(function(source,n) for n in ('mqtt_set_cache_flush_paused','mqtt_flush_offline_cache')))
        local_mode.LocalModeTests.compile_run(self,code)

    def test_background_ota_quiet_does_not_pause_dsp(self):
        ota=(ROOT/'main/ota/ota_service.c').read_text(encoding='utf-8')
        self.assertNotIn('arm_poll_set_quiet',function(ota,'ota_quiet_begin'))
        self.assertNotIn('arm_poll_set_quiet',function(ota,'ota_quiet_end'))
        dsp=(ROOT/'main/direct/direct_poll.c').read_text(encoding='utf-8')
        self.assertIn('xTaskCreate(dsp_task, "direct_dsp", 4096, NULL, 6,',dsp)
        self.assertIn('#define OTA_TASK_PRIORITY     5U',ota)


if __name__=='__main__':
    unittest.main()
