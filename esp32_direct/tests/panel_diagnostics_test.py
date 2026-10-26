"""Exercise the real observer while the panel/SPI owner is stalled."""
from pathlib import Path
import unittest
from mqtt_connection_window_test import function
import mqtt_local_mode_test as local_mode

ROOT = Path(__file__).resolve().parents[1]


class PanelDiagnosticsTests(unittest.TestCase):
    def test_stalled_owner_is_visible_without_an_extra_task_and_healthy_logs_are_throttled(self):
        source = (ROOT / 'main/direct/direct_panel.c').read_text(encoding='utf-8')
        declarations = source[source.index('typedef enum {'):source.index('#if !CONFIG_DIRECT_LCD_GPIO_SPI')]
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
static atomic_uint s_lcd_tx_errors,s_panel_tick;
static atomic_bool s_first_frame_done;
DECLARATIONS
static unsigned clock_ms=1000, reports;
static const char *TAG="test";
static char last[512];
static unsigned now_ms(void){return clock_ms;}
static void capture(const char *tag,const char *fmt,...){
 (void)tag;va_list ap;va_start(ap,fmt);vsnprintf(last,sizeof(last),fmt,ap);va_end(ap);reports++;
}
#define ESP_LOGW capture
FUNCTIONS
int main(void){
 panel_stage(PANEL_SPLASH);atomic_store(&s_spi_active,true);
 atomic_store(&s_spi_started_ms,clock_ms);atomic_store(&s_spi_bytes,10240);
 atomic_store(&s_panel_row,80);
 atomic_store(&s_panel_page,3);atomic_store(&s_panel_key_events,7);atomic_store(&s_panel_frames,2);
 direct_panel_diagnostic_tick();assert(reports==1&&strstr(last,"stage=SPLASH")&&strstr(last,"spi=1"));
 assert(strstr(last,"page=3 keys=7 frames=2"));
 clock_ms+=4999;direct_panel_diagnostic_tick();assert(reports==1);
 clock_ms++;direct_panel_diagnostic_tick();assert(reports==2);
 assert(strstr(last,"stage_age=5000")&&strstr(last,"spi_age=5000")&&strstr(last,"row=80"));
 atomic_store(&s_spi_active,false);panel_stage(PANEL_SNAPSHOT);
 clock_ms+=5000;direct_panel_diagnostic_tick();assert(reports==3&&strstr(last,"stage=SNAPSHOT"));
 atomic_store(&s_first_frame_done,true);atomic_store(&s_panel_tick,clock_ms);
 clock_ms+=5000;atomic_store(&s_panel_tick,clock_ms);panel_stage(PANEL_WAIT);
 direct_panel_diagnostic_tick();assert(reports==4&&strstr(last,"first=1")&&strstr(last,"spi=0"));
 clock_ms+=29999;direct_panel_diagnostic_tick();assert(reports==4);
 clock_ms++;direct_panel_diagnostic_tick();assert(reports==5&&strstr(last,"loop_age=30000"));
 clock_ms+=5000;direct_panel_diagnostic_tick();assert(reports==6);
 puts("PASS independent observer exposes stalled splash/SPI/snapshot and a stopped refresh loop; healthy reports throttled");
}
'''.replace('DECLARATIONS', declarations).replace('FUNCTIONS', '\n'.join(
            function(source, name) for name in ('panel_stage', 'direct_panel_diagnostic_tick')))
        local_mode.LocalModeTests.compile_run(self, code)


if __name__ == '__main__':
    unittest.main()
