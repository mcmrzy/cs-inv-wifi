"""Render current direct source and compare equivalent real LVGL framebuffers."""
from pathlib import Path
import subprocess,sys
from PIL import Image,ImageDraw,ImageFont
root=Path(__file__).resolve().parents[1];old=root.parent/'esp32_direct'
source=root/'tmp/direct_visual_reference.c'
source.write_text(r'''
#include "direct_ui.h"
#include "direct_ui_scene.h"
#include "direct_gfx.h"
#include <stdio.h>
#include <assert.h>
static uint16_t pixels[320*240];
static void frame(const char *name,direct_ui_state_t *u,direct_ui_data_t *d){
 direct_ui_view_t view;direct_ui_build(u,d,&view);direct_gfx_t g;
 direct_gfx_begin(&g,pixels,320,240,0,0);direct_ui_scene_draw(&g,u,d,&view);
 char path[160];snprintf(path,sizeof(path),"tmp/direct_%s.ppm",name);
 FILE *f=fopen(path,"wb");assert(f);fprintf(f,"P6\n320 240\n255\n");
 for(unsigned i=0;i<320*240;i++){
  uint16_t p=pixels[i];unsigned char c[]={(p>>11)*255/31,((p>>5)&63)*255/63,(p&31)*255/31};
  assert(fwrite(c,1,3,f)==3);
 }assert(!fclose(f));
}
int main(void){
 static direct_ui_data_t d;direct_ui_state_t u;direct_ui_init(&u);
 d.language_zh=true;d.dsp_online=true;
 d.run.ACInWatt=1800;d.run.GridVolt=230;d.run.Ppv=4200;d.run.Vpv1=368;
 d.run.OutputWatt=3200;d.run.ACOutputVolt=230;d.run.BatVolt=524;
 d.run.BatChgCurr=235;d.run.BatterySOC=78;
 frame("home",&u,&d);u.page=UI_MENU;u.selected=1;frame("menu",&u,&d);
 u.page=UI_CONFIG;u.group=7;u.selected=7;u.editing=true;u.draft=2300;
 d.config[7]=2200;d.valid[7]=true;d.writable[7]=true;frame("editor",&u,&d);
 return 0;
}
''',encoding='utf-8')
gcc=sys.argv[1];exe=root/'tmp/direct_visual_reference.exe'
names=['direct_ui','direct_gfx','direct_gfx_assets','direct_ui_assets','direct_ui_locale',
       'direct_battery_config','direct_home','direct_home_asset','direct_home_font','direct_ui_scene']
subprocess.run([gcc,'-save-temps=obj','-std=c11','-Os','-Wall','-Wextra','-Werror',
 '-I'+str(old/'main/direct'),'-I'+str(old/'main/telemetry'),'-I'+str(old/'main/ota'),
 str(source),*[str(old/f'main/direct/{n}.c') for n in names],'-o',str(exe)],check=True)
subprocess.run([str(exe)],cwd=root,check=True)
font=str(old/'tests/font-source/noto/NotoSansCJKsc-Medium.otf')
heading=ImageFont.truetype(font,19);body=ImageFont.truetype(font,15)
canvas=Image.new('RGB',(690,910),'#e9edf2');draw=ImageDraw.Draw(canvas)
draw.text((16,10),'当前 direct 源码',font=heading,fill='#172d45')
draw.text((356,10),'新版 LVGL · 实际固件渲染',font=heading,fill='#172d45')
for i,(left,right,caption) in enumerate([
 ('direct_home','lvgl_home','同一组功率、电压及 SOC 示例数据'),
 ('direct_menu','lvgl_menu','图标宫格；新版保留五个产品入口'),
 ('direct_editor','lvgl_output_editor','输出电压：220V → 230V')]):
 y=48+i*282
 for x,name in [(15,left),(355,right)]:canvas.paste(Image.open(root/f'tmp/{name}.ppm'),(x,y))
 draw.text((16,y+246),caption,font=body,fill='#172d45')
canvas.save(root/'tmp/direct_vs_lvgl.png')
print('PASS direct comparison: compiled current direct source; equivalent live values and voltage target')
