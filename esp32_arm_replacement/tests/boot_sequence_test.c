#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "direct_panel.h"
#include "direct_splash.h"
#include "direct_boot_logo.h"
#include "esp_log.h"
#define CONFIG_DIRECT_LCD_GPIO_SPI 1
#define CONFIG_DIRECT_LCD_NOCS_BOOT 1
#define CONFIG_DIRECT_LCD_MINIMAL_INIT 1
#define CONFIG_DIRECT_LCD_CS_FIXED_LOW 1
#define CONFIG_DIRECT_LCD_4WIRE 1
#define CONFIG_DIRECT_BOOT_DIAGNOSTICS 0
#define CONFIG_DIRECT_LCD_READBACK_TERMINAL 0
#define CONFIG_DIRECT_LCD_COLOR_TEST 0
#define CONFIG_DIRECT_LCD_SCLK_PIN 6
#define CONFIG_DIRECT_LCD_MOSI_PIN 7
#define CONFIG_DIRECT_LCD_DC_PIN 9
#define CONFIG_DIRECT_LCD_RST_PIN 10
#define CONFIG_DIRECT_LCD_CS_PIN 4
#define CONFIG_DIRECT_LCD_BL_PIN -1
#define GPIO_MODE_OUTPUT 1
#define ESP_OK 0
#define ESP_ERROR_CHECK(call) assert((call)==0)
#define LCD_W 320
#define LCD_H 240
#define BLACK 0
#define pdMS_TO_TICKS(ms) (ms)
#define pdPASS 1
typedef struct {uint64_t pin_bit_mask;int mode;} gpio_config_t;
static const char *TAG="test";
static uint16_t frame[320*240];
/* The splash must borrow the panel owner's band, not reserve a second 10KB. */
static struct {
 uint32_t before;
 uint16_t pixels[320*16];
 uint32_t after;
} panel_band;
uint16_t *direct_panel_band_buffer(void){return panel_band.pixels;}
static bool cleared[15],visible,light;
static unsigned shows,early_home,task_created,hold_captures;
static int64_t clock_us;
static int direct_keys_init(void){return 0;}
static int gpio_config(const gpio_config_t *config){(void)config;return 0;}
static int gpio_set_level(int pin,int level){(void)pin;(void)level;return 0;}
static uint64_t pin_mask(int pin){return pin<0?0:1ULL<<pin;}
static bool direct_lcd_cs_toggles(int pin,int fixed){return pin>=0&&!fixed;}
static void command(uint8_t value){if(value==0x29){shows++;visible=true;}if(value==0x28)visible=false;}
static void data8(uint8_t value){(void)value;}
static void fill_rect(int x,int y,int w,int h,uint16_t color){
 assert(!visible);assert(x==0&&y==0&&w==320&&h==240);(void)color;
}
static void panel_render(void){early_home++;}
static void panel_task(void *arg){(void)arg;}
static int xTaskCreate(void (*task)(void*),const char *name,unsigned stack,void *arg,int prio,void *handle){
 (void)name;(void)stack;(void)arg;(void)prio;(void)handle;assert(task==panel_task);task_created++;return pdPASS;
}
int64_t esp_timer_get_time(void){return clock_us;}
static void verify_logo_only(void){
 const int ox=(320-direct_boot_logo_width)/2, oy=(240-direct_boot_logo_height)/2;
 const uint16_t bg=light?0xf7bf:0x0085;
 const uint16_t ink[2]={light?0x19f1:0x4c3f,light?0x0085:0xf7bf};
 for(int y=0;y<240;y++)for(int x=0;x<320;x++){
  uint16_t expected=bg;
  for(unsigned part=0;part<2;part++){
   const direct_boot_logo_part_t *p=&direct_boot_logo_parts[part];
   int px=x-ox-p->x_offset,py=y-oy-p->y_offset;
   if(px>=0&&py>=0&&px<p->width&&py<p->height &&
      (p->data[py*p->stride+(px>>3)]&(0x80>>(px&7))))expected=ink[part];
  }
  assert(frame[y*320+x]==expected);
 }
}
static void capture(void){
 verify_logo_only();
 char path[80];snprintf(path,sizeof(path),"tmp/boot_stacked_%s.ppm",light?"light":"dark");
 FILE *f=fopen(path,"wb");assert(f);fprintf(f,"P6\n320 240\n255\n");
 for(unsigned i=0;i<320*240;i++){unsigned p=frame[i];unsigned char rgb[]={((p>>11)&31)*255/31,((p>>5)&63)*255/63,(p&31)*255/31};assert(fwrite(rgb,1,3,f)==3);}fclose(f);
}
void vTaskDelay(unsigned ms){clock_us+=ms*1000;if(ms==500){capture();hold_captures++;}}
void direct_panel_show_band(int y,uint16_t *pixels){
 assert(pixels==direct_panel_band_buffer());
 assert(y>=0&&y%16==0&&y+16<=240);memcpy(frame+y*320,pixels,320*16*2);cleared[y/16]=true;
 /* Model the real driver consuming and byte-swapping the borrowed band. */
 for(unsigned i=0;i<320*16;i++)pixels[i]=(uint16_t)((pixels[i]>>8)|(pixels[i]<<8));
}
void direct_panel_enable_display(void){
 for(unsigned i=0;i<15;i++)assert(cleared[i]);
 uint16_t bg=light?0xf7bf:0x0085;
 for(unsigned i=0;i<320*240;i++)assert(frame[i]==bg);
 command(0x29);
}
#include "../tmp/boot_init_under_test.inc"
int main(void){
 (void)panel_render;
 assert(direct_boot_logo_width==276 && direct_boot_logo_parts[1].width==176);
 assert(direct_boot_logo_parts[1].y_offset>=direct_boot_logo_parts[0].height+18);
 for(unsigned theme=0;theme<2;theme++){
  panel_band.before=0x13579bdf;panel_band.after=0x2468ace0;
  memset(panel_band.pixels,0xa5,sizeof(panel_band.pixels));
  light=theme;memset(cleared,0,sizeof(cleared));memset(frame,0xa5,sizeof(frame));visible=false;shows=early_home=task_created=hold_captures=0;
  direct_panel_init();assert(!visible && shows==0 && early_home==0 && task_created==1);
  direct_splash_run(light);assert(shows==1 && hold_captures==1);
  assert(panel_band.before==0x13579bdf && panel_band.after==0x2468ace0);
  uint16_t bg=light?0xf7bf:0x0085;for(unsigned i=0;i<320*240;i++)assert(frame[i]==bg);
 }
 puts("PASS: actual panel init never paints home or enables scanout; splash reuses panel band with guards intact despite driver byte-swap; clears 15 bands before DISPON; centred logo-only dark/light pixels; fade-out clears all pixels");
}
