#include "direct_onboarding_scene.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint16_t full[320*240],strip[320*16];
static void export_ppm(const char *path) {
    FILE*f=fopen(path,"wb");assert(f);fprintf(f,"P6\n320 240\n255\n");
    for(int i=0;i<320*240;i++){uint16_t p=full[i];unsigned char rgb[3]={(unsigned char)(((p>>11)&31)*255/31),(unsigned char)(((p>>5)&63)*255/63),(unsigned char)((p&31)*255/31)};fwrite(rgb,1,3,f);}fclose(f);
}
int main(int argc,char **argv) {
    onboarding_qr_t q={0};assert(onboarding_qr_update(&q,"H1ZZX0013900001P","001234"));
    for(int light=0;light<2;light++)for(int zh=0;zh<2;zh++) {
        direct_gfx_t g;direct_gfx_begin(&g,full,320,240,0,0);g.theme_light=light;g.language_zh=zh;
        direct_onboarding_scene_draw(&g,&q,"H1ZZX0013900001P","001234",false,false);
        for(int y=0;y<240;y+=16){direct_gfx_begin(&g,strip,320,16,y,0);g.theme_light=light;g.language_zh=zh;direct_onboarding_scene_draw(&g,&q,"H1ZZX0013900001P","001234",false,false);assert(memcmp(strip,full+y*320,sizeof(strip))==0);}
        if(argc>1){char path[240];snprintf(path,sizeof(path),"%s_%s_%s.ppm",argv[1],light?"light":"dark",zh?"zh":"en");export_ppm(path);}
    }
    puts("onboarding scene: PASS (full versus 16px strips, both themes/languages)");return 0;
}
