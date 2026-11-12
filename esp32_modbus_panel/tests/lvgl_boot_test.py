"""Exercise the real LVGL startup block through the real LCD flush and DISPON."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
runtime = (root / 'full/full_panel_runtime.inc').read_text(encoding='utf-8')
driver = (root / 'full/direct_panel.c').read_text(encoding='utf-8')
task = runtime[runtime.index('static void panel_task('):]
start = task.index('    if(s_panel_initialized){\n        panel_lvgl_splash(')
startup = task[start:task.index('    s_have_previous=false;s_ui.dirty=true;', start)]
flush = runtime[runtime.index('static void panel_write_pixels('):runtime.index('static bool panel_set_view_inversion(')]
enable = driver[driver.index('void direct_panel_enable_display(void)'):driver.index('#if CONFIG_DIRECT_LCD_ISOLATION_TEST', driver.index('void direct_panel_enable_display(void)'))]
assert 'command(0x28); /* Keep scanout off' in driver
test = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#define LCD_W 320
#define LCD_H 240
#define CONFIG_DIRECT_LCD_GPIO_SPI 0
#define pdMS_TO_TICKS(v) (v)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
static atomic_uint s_lcd_tx_errors,s_panel_row,s_panel_tick;
enum {PANEL_SPLASH_DURATION_MS=1700};
static bool s_panel_initialized=true;
static unsigned s_saved_style=2,frames,pixel_bytes,display_on,backlight,fail_band,enable_failure;
static unsigned window_bytes;
static uint16_t pixels[LCD_W*16];
static uint32_t clock_ms=123;
static uint32_t now_ms(void){return clock_ms;}
static void vTaskDelay(unsigned delay){clock_ms+=delay;}
static void command(uint8_t value){
 assert(value==0x29);assert(frames==15 && pixel_bytes==320*240*2);
 display_on++;if(enable_failure)atomic_fetch_add(&s_lcd_tx_errors,1);
}
static void full_backlight_attach(void){assert(display_on);backlight++;}
static void window(int x,int y,int w,int h){
 assert(x==0 && y>=0 && y<240 && w==320 && h==16);window_bytes=(unsigned)w*h*2;
}
static void tx(bool data,const void *bytes,size_t count){
 assert(data && count==window_bytes);const uint8_t *wire=bytes;
 assert(wire[0]==0x12 && wire[1]==0x34);pixel_bytes+=(unsigned)count;
 if(++frames==fail_band)atomic_fetch_add(&s_lcd_tx_errors,1);
}
FLUSH
ENABLE
static void panel_lvgl_splash(bool light){assert(!light);}
static void panel_lvgl_refresh(uint32_t now,bool full){
 assert(now==clock_ms);
 for(int y=0;y<(full?240:16);y+=16){
  for(unsigned i=0;i<320*16;i++)pixels[i]=0x1234;
  panel_write_pixels(0,y,320,16,pixels);
 }
}
static void startup(void){STARTUP}
static void reset(void){
 frames=pixel_bytes=display_on=backlight=fail_band=enable_failure=0;
 clock_ms=123;
 s_panel_initialized=true;atomic_store(&s_lcd_tx_errors,0);
}
int main(void){
 reset();startup();assert(display_on==1 && backlight==1 && s_panel_initialized);
 assert(clock_ms>=143+PANEL_SPLASH_DURATION_MS && clock_ms<143+PANEL_SPLASH_DURATION_MS+30);
 assert(frames>15 && atomic_load(&s_panel_tick)==clock_ms);
 reset();fail_band=3;startup();assert(!display_on && !backlight && !s_panel_initialized);
 reset();enable_failure=1;startup();assert(display_on==1 && !s_panel_initialized);
 reset();fail_band=20;startup();assert(display_on==1 && !s_panel_initialized && frames==20);
 reset();s_panel_initialized=false;startup();assert(!frames && !display_on);
 puts("PASS LVGL boot: actual RGB565 flush, complete frame before DISPON/backlight, timed animation, SPI failures retained for recovery");
}
'''.replace('FLUSH', flush).replace('ENABLE', enable).replace('STARTUP', startup)
path = root / 'tmp/lvgl_boot_test.c'
path.parent.mkdir(exist_ok=True)
path.write_text(test, encoding='utf-8')
exe = path.with_suffix('.exe')
subprocess.run([sys.argv[1], '-save-temps=obj', '-std=c11', '-Wall', '-Wextra', '-Werror', str(path), '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True)
