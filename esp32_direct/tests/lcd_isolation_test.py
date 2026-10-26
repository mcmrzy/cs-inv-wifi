"""Run the real isolation loop with an instrumented framebuffer and SDK stubs."""
from pathlib import Path
import unittest
from mqtt_connection_window_test import function
import mqtt_local_mode_test as local_mode

ROOT = Path(__file__).resolve().parents[1]


class LcdIsolationTests(unittest.TestCase):
    def test_sequence_pixels_hold_and_ota_confirmation(self):
        source = (ROOT / 'main/direct/direct_panel.c').read_text(encoding='utf-8')
        harness = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#define LCD_W 320
#define LCD_H 240
#define BLACK 0
#define WHITE 65535
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_OTA_IMG_PENDING_VERIFY 1
#define ESP_ERROR_CHECK(e) do {if ((e)!=0) longjmp(done,2);} while(0)
#define ESP_LOGI(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
typedef int esp_ota_img_states_t;
typedef int esp_partition_t;
static jmp_buf done;
static atomic_uint s_lcd_tx_errors,s_spi_transfers;
static uint16_t fb[LCD_H][LCD_W];
static unsigned holds,shown,confirmed,writes;
static bool pending=true,inject_error;
static void fill_rect(int x,int y,int w,int h,uint16_t c){
 assert(x>=0&&y>=0&&x+w<=LCD_W&&y+h<=LCD_H);
 for(int j=y;j<y+h;j++)for(int i=x;i<x+w;i++)fb[j][i]=c;
 writes+=(unsigned)(w*h);
 if(inject_error)atomic_store(&s_lcd_tx_errors,1);
}
static void direct_panel_enable_display(void){shown++;}
static const esp_partition_t *esp_ota_get_running_partition(void){static int p;return &p;}
static int esp_ota_get_state_partition(const int *p,int *s){assert(p);*s=pending?1:2;return 0;}
static int esp_ota_mark_app_valid_cancel_rollback(void){confirmed++;return 0;}
static void vTaskDelay(unsigned ms){
 const uint16_t colors[]={0xf800,0x07e0,0x001f,0,65535};
 assert(ms==2000&&shown==1&&writes==LCD_W*LCD_H);
 for(int y=0;y<LCD_H;y++)for(int x=0;x<LCD_W;x++){
  uint16_t expected=holds%6<5?colors[holds%6]:(((x/16+y/16)&1)?BLACK:WHITE);
  assert(fb[y][x]==expected);
 }
 writes=0;
 if(++holds==12)longjmp(done,1);
}
FUNCTION
int main(void){
 for(int mode=0;mode<3;mode++){
  holds=shown=confirmed=writes=0;atomic_store(&s_lcd_tx_errors,0);
  pending=mode!=1;inject_error=mode==2;
  int result=setjmp(done);
  if(!result)panel_isolation_loop();
  if(inject_error)assert(result==2&&holds==0&&confirmed==0);
  else assert(result==1&&holds==12&&confirmed==(pending?1U:0U));
 }
 puts("PASS two full pattern cycles, every pixel, 2s hold, OTA pending-only confirmation and SPI failure guard");
}
'''.replace('FUNCTION', function(source, 'panel_isolation_loop'))
        local_mode.LocalModeTests.compile_run(self, harness)


if __name__ == '__main__':
    unittest.main()
