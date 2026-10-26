"""Real recovery policy and owner service; mock only GPIO/SDK boundaries."""
from pathlib import Path
import unittest
from mqtt_connection_window_test import function
import mqtt_local_mode_test as local_mode

ROOT = Path(__file__).resolve().parents[1]


class LcdRecoveryTests(unittest.TestCase):
    def header(self):
        header = ROOT / 'main/direct/direct_lcd_recovery.h'
        self.assertTrue(header.exists(), 'LCD recovery policy has not been implemented')
        return '#include "' + header.as_posix() + '"\n'

    def test_actual_scanner_ignores_long_combo_but_normal_enter_still_works(self):
        keys=(ROOT/'main/direct/direct_keys.c').read_text(encoding='utf-8')
        codec=(ROOT/'main/direct/direct_key_codec.c').as_posix()
        code = self.header()+'#include "'+codec+'"\n'+r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <setjmp.h>
#include <string.h>
typedef unsigned TickType_t;
typedef struct {uint8_t key;uint32_t timestamp_ms;} key_event_t;
static void *s_events=(void*)1;
static atomic_bool s_lcd_recovery_requested;
static const char *TAG="test";
static unsigned clock_ms,beeps,queued,recovery_logs;
static jmp_buf stop;
#define CONFIG_DIRECT_KEY_CLOCK_PIN 18
#define CONFIG_DIRECT_KEY_DATA_PIN 19
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGI capture
#define ESP_LOGW capture
static void capture(const char *tag,const char *format,...){(void)tag;if(strstr(format,"LCD_RECOVERY"))recovery_logs++;}
static unsigned xTaskGetTickCount(void){return clock_ms;}
static int64_t esp_timer_get_time(void){return (int64_t)clock_ms*1000;}
static int read_bs818a(uint16_t *raw){*raw=0;return clock_ms<4200?7:clock_ms<4400?0:2;}
static unsigned uxQueueSpacesAvailable(void *q){(void)q;return 32;}
static unsigned uxQueueMessagesWaiting(void *q){(void)q;return 0;}
static int gpio_get_level(int pin){(void)pin;return 0;}
static int xQueueSend(void *q,const key_event_t *event,unsigned wait){(void)q;(void)wait;assert(event->key==2);queued++;return pdTRUE;}
static void direct_buzzer_pulse(void){beeps++;}
static void vTaskDelayUntil(TickType_t *wake,unsigned delay){*wake+=delay;clock_ms=*wake;if(clock_ms>=5000)longjmp(stop,1);}
FUNCTION
int main(void){
 if(setjmp(stop)==0)keys_task(NULL);
 assert(recovery_logs==0&&"ENTER+BACK must not request LCD recovery");
 assert(!atomic_load(&s_lcd_recovery_requested));
 assert(beeps==1&&queued==1); /* one normal ENTER after releasing the combo */
 puts("PASS actual scanner ignores >4s ENTER+BACK combo; normal ENTER press still queues and beeps once");
}
'''.replace('FUNCTION',function(keys,'keys_task'))
        local_mode.LocalModeTests.compile_run(self, code)

    def test_actual_owner_bounds_auto_retries_defers_ota_and_keeps_manual_available(self):
        runtime = (ROOT/'main/direct/direct_panel_runtime.inc').read_text(encoding='utf-8')
        panel = (ROOT/'main/direct/direct_panel.c').read_text(encoding='utf-8')
        code = self.header() + r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
static direct_lcd_recovery_t s_recovery;
static bool s_panel_initialized=true,s_manual_recovery_pending;
static atomic_uint s_lcd_tx_errors,s_recovery_error_baseline,s_panel_tick;
static atomic_bool s_first_frame_done;
static unsigned clock_ms,resets;static bool ota,fail;
static unsigned now_ms(void){return clock_ms;}
static bool ota_service_is_busy(void){return ota;}
static bool direct_keys_get_timed_event(uint8_t *key,uint32_t *ms){(void)key;(void)ms;return false;}
static bool panel_reset_display(void){resets++;s_panel_initialized=!fail;return !fail;}
FUNCTIONS
int main(void){
 for(unsigned n=0;n<10000;n+=20){clock_ms=n;panel_service_recovery();}
 assert(resets==0); /* static screen is not a failure */
 atomic_store(&s_first_frame_done,true);atomic_store(&s_panel_tick,clock_ms);
 assert(direct_panel_software_healthy());
 atomic_store(&s_lcd_tx_errors,1);fail=true;panel_service_recovery();assert(resets==1);
 clock_ms+=2999;panel_service_recovery();assert(resets==1);
 clock_ms++;ota=true;panel_service_recovery();assert(resets==1);
 ota=false;panel_service_recovery();assert(resets==2);
 clock_ms+=100000;panel_service_recovery();assert(resets==2); /* retry budget */
 s_manual_recovery_pending=true;ota=true;panel_service_recovery();assert(resets==2&&s_manual_recovery_pending);
 ota=false;fail=false;panel_service_recovery();assert(resets==3&&!s_manual_recovery_pending);
 assert(atomic_load(&s_recovery_error_baseline)==1); /* historical errors do not poison recovery */
 atomic_store(&s_first_frame_done,true);atomic_store(&s_panel_tick,clock_ms);
 assert(direct_panel_software_healthy());
 clock_ms+=4000;panel_service_recovery();assert(resets==3);
 /* A failed first initialization still enters the same retry/manual service. */
 s_recovery=(direct_lcd_recovery_t){0};s_panel_initialized=false;clock_ms=4000;
 panel_service_recovery();assert(resets==4&&s_panel_initialized);
 puts("PASS actual owner service: bounded retries, cooldown, OTA deferral, manual retry after exhaustion, recovered health, initial failure");
}
'''.replace('FUNCTIONS', function(runtime,'panel_service_recovery')+'\n'+function(runtime,'direct_panel_software_healthy'))
        self.assertIn('panel_service_recovery();', function(runtime,'panel_task'))
        self.assertIn('panel_hardware_initialize()', function(panel,'direct_panel_init'))
        local_mode.LocalModeTests.compile_run(self, code)

    def test_actual_reset_forces_repaint_preserves_ui_state_and_reports_enable_failure(self):
        runtime = (ROOT/'main/direct/direct_panel_runtime.inc').read_text(encoding='utf-8')
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
static atomic_uint s_lcd_tx_errors;
static atomic_bool s_first_frame_done;
static bool s_panel_initialized,s_blank=true,s_have_previous=true,s_lcd_test_inverted=true;
static struct {bool dirty;int page,draft;bool pending;} s_ui={false,8,123,true};
static unsigned hardware_calls,enable_calls;static bool fail_hardware,fail_enable;
static bool panel_hardware_initialize(void){hardware_calls++;if(fail_hardware)atomic_fetch_add(&s_lcd_tx_errors,1);return !fail_hardware;}
static void direct_panel_enable_display(void){enable_calls++;if(fail_enable)atomic_fetch_add(&s_lcd_tx_errors,1);}
FUNCTION
int main(void){
 atomic_store(&s_first_frame_done,true);
 assert(panel_reset_display());assert(hardware_calls==1&&enable_calls==1);
 assert(!s_blank&&!s_have_previous&&!s_lcd_test_inverted&&s_ui.dirty);
 assert(!atomic_load(&s_first_frame_done)&&s_ui.page==8&&s_ui.draft==123&&s_ui.pending);
 fail_hardware=true;assert(!panel_reset_display());assert(enable_calls==1);
 fail_hardware=false;fail_enable=true;assert(!panel_reset_display());assert(enable_calls==2);
 puts("PASS real reset forces first-frame/full repaint, preserves pending UI edits and detects hardware/display-enable errors");
}
'''.replace('FUNCTION',function(runtime,'panel_reset_display'))
        local_mode.LocalModeTests.compile_run(self,code)


if __name__ == '__main__':
    unittest.main()
