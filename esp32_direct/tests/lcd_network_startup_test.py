"""Run the real startup gate and real network-start tail at SDK boundaries."""
from pathlib import Path
import unittest
from mqtt_connection_window_test import function
import mqtt_local_mode_test as local_mode

ROOT=Path(__file__).resolve().parents[1]


class LcdNetworkStartupTests(unittest.TestCase):
    def test_actual_startup_waits_for_ui_then_releases_radio_or_times_out(self):
        app=(ROOT/'main/app/app_main.c').read_text(encoding='utf-8')
        gate=function(app,'wait_for_first_ui_frame')
        main=function(app,'app_main')
        start=main.index('    offline_cache_init();')
        end=main.index('    /* Reuse the startup task',start)
        tail=main[start:end]
        self.assertLess(main.index('ota_service_init()'),start)
        self.assertLess(main.index('mqtt_init('),start)
        self.assertLess(main.index('factory_service_start()'),start)
        self.assertLess(main.index('xTaskCreate(main_loop_task'),start)
        loop=function(app,'main_loop_task')
        self.assertLess(loop.index('ulTaskNotifyTake('),loop.index('esp_task_wdt_add('))
        self.assertLess(tail.index('wait_for_first_ui_frame()'),tail.index('wifi_sta_init()'))
        code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_LOGI capture
#define ESP_LOGW capture
#define LCD_FIRST_UI_WAIT_MS 5000U
#define BLE_STARTUP_WAIT_MS 30000U
#define CLOUD_RETRY_WINDOW_MS 30000U
#define STATE_INIT 0
#define BOOT_READY 1
static const char *TAG="test";
static atomic_int s_boot_stage;
static uint32_t clock_ms,scenario_start,ready_after;
static uint32_t s_ble_start_deadline_ms,s_cloud_retry_deadline_ms,s_next_ble_start_ms;
static void *s_main_loop_handle=(void*)1;
static unsigned sleeps,diagnostics,ready_logs,timeout_logs;
static char events[32];static unsigned used;
static void record(char c){assert(used+1<sizeof(events));events[used++]=c;events[used]=0;}
static void capture(const char *tag,const char *format,...){
 (void)tag;
 if(strstr(format,"LCD_NETWORK_GATE waiting"))record('G');
 if(strstr(format,"LCD_NETWORK_GATE ready")){record('R');ready_logs++;}
 if(strstr(format,"LCD_NETWORK_GATE timeout")){record('T');timeout_logs++;}
}
static uint32_t now_ms(void){return clock_ms;}
static bool direct_panel_software_healthy(void){return ready_after!=UINT32_MAX&&(uint32_t)(clock_ms-scenario_start)>=ready_after;}
static void direct_panel_diagnostic_tick(void){diagnostics++;}
static void vTaskDelay(unsigned delay){assert(delay==20);clock_ms+=delay;sleeps++;assert(sleeps<=250);}
static void offline_cache_init(void){record('O');}
static void wifi_sta_init(void){assert(ready_logs||timeout_logs);record('W');}
static void setup_wifi(void){record('C');}
static void mqtt_connect(void){record('M');}
static void sntp_sync_init(void){record('S');}
static void enter_state(unsigned state){assert(state==STATE_INIT);record('E');}
static void xTaskNotifyGive(void *handle){assert(handle==s_main_loop_handle);record('N');}
/* INSERT_GATE */
static void start_network(void){
/* INSERT_TAIL */
}
static void run(unsigned ready,uint32_t initial){
 clock_ms=scenario_start=initial;ready_after=ready;
 sleeps=diagnostics=ready_logs=timeout_logs=used=0;events[0]=0;
 atomic_store(&s_boot_stage,0);
 start_network();
 assert(atomic_load(&s_boot_stage)==BOOT_READY);
 assert(s_ble_start_deadline_ms==clock_ms+BLE_STARTUP_WAIT_MS);
 assert(s_cloud_retry_deadline_ms==clock_ms+CLOUD_RETRY_WINDOW_MS);
 assert(s_next_ble_start_ms==clock_ms);
 if(ready==UINT32_MAX){assert(clock_ms-scenario_start==5000U);assert(timeout_logs==1&&!ready_logs);assert(!strcmp(events,"OGTWCMSEN"));}
 else{assert(clock_ms-scenario_start==ready);assert(ready_logs==1&&!timeout_logs);assert(!strcmp(events,"OGRWCMSEN"));}
}
int main(void){
 run(700,0);assert(diagnostics>0&&sleeps>0);
 run(0,0);assert(sleeps==0);
 run(UINT32_MAX,0);assert(sleeps==250&&diagnostics>0);
 run(700,UINT32_MAX-200U);
 run(UINT32_MAX,UINT32_MAX-200U);
 puts("PASS actual startup tail: UI before WiFi/MQTT/main-loop release, ready/immediate/5s timeout/wrap, network timers begin after gate");
}
'''.replace('/* INSERT_GATE */',gate).replace('/* INSERT_TAIL */',tail)
        local_mode.LocalModeTests.compile_run(self,code)


if __name__=='__main__':
    unittest.main()
