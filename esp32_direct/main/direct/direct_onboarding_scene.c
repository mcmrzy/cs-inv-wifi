#include "direct_onboarding_scene.h"
#include <stdio.h>
static void text(direct_gfx_t *g,int x,int y,const char *zh,const char *en,int size,uint16_t color) {
    if(y+24<=g->y0 || y>=g->y0+g->height)return;
    direct_gfx_text(g,x,y,g->language_zh?zh:en,size,color);
}
void direct_onboarding_scene_draw(direct_gfx_t *g,const onboarding_qr_t *q,const char *sn,const char *pin,bool has_wifi,bool bound) {
    if(!g)return;
    uint16_t bg=g->theme_light?0xf7bf:0x0085,fg=g->theme_light?0x08e8:0xffff;
    uint16_t blue=g->theme_light?0x02f3:0x07ff,green=g->theme_light?0x03c7:0x07e0;
    direct_gfx_rect(g,0,0,320,240,bg);
    text(g,12,8,"连接辰烁 App","Connect CSERGY App",16,fg);
    /* White panel is invariant across themes. Size/version bound gives >=3px/module. */
    direct_gfx_rect(g,8,39,156,156,0xffff);
    int size=onboarding_qr_size(q);
    if(size>0) {
        int scale=156/(size+8),span=(size+8)*scale;
        int x0=8+(156-span)/2+4*scale,y0=39+(156-span)/2+4*scale;
        for(int y=0;y<size;y++) {
            if(y0+(y+1)*scale<=g->y0 || y0+y*scale>=g->y0+g->height)continue;
            for(int x=0;x<size;x++)if(onboarding_qr_module(q,x,y))direct_gfx_rect(g,x0+x*scale,y0+y*scale,scale,scale,0x0000);
        }
    } else {
        text(g,20,96,"设备识别码","Device identity",12,0x08e8);
        text(g,20,118,"未配置","Not configured",12,0x02f3);
    }
    /* Small phone illustration; does not intrude into the QR quiet zone. */
    direct_gfx_rect(g,176,39,24,36,blue);direct_gfx_rect(g,179,42,18,27,bg);
    direct_gfx_rect(g,185,71,6,2,blue);
    text(g,208,44,"扫一扫","Scan QR",14,blue);
    text(g,176,84,"1 打开手机 App","1 Open the App",12,fg);
    text(g,176,109,has_wifi?"2 添加设备":"2 配网并添加",has_wifi?"2 Add device":"2 Set up Wi-Fi",12,fg);
    text(g,176,138,"配对 PIN","Pairing PIN",12,blue);
    if(q && q->ready && pin)direct_gfx_text(g,176,159,pin,22,fg);
    else text(g,176,160,"暂不可扫码","QR unavailable",12,fg);
    text(g,176,187,bound?"已绑定":"正常使用不受影响",bound?"Device linked":"Use normally",12,green);
    char serial[24];snprintf(serial,sizeof(serial),"SN %s",sn?sn:"--");
    direct_gfx_text(g,12,207,serial,12,fg);
    text(g,12,225,"返回：稍后设置","Back: set up later",12,blue);
}
