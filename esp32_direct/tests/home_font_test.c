#include "direct_gfx.h"
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef HOME_FONT_LEGACY
/* Exercise the old renderer to demonstrate why the home page needs native sizes. */
#define direct_home_text_width direct_gfx_text_width
#define direct_home_text direct_gfx_text
#else
#include "direct_home_font.h"
#endif

enum { W=192,H=64,GUARD=16 };
static uint16_t full[W*H+2*GUARD],strip[W*7+2*GUARD];
static void fill(uint16_t *p,size_t n,uint16_t c)
{for(size_t i=0;i<n;i++)p[i]=c;}
static void guards(const uint16_t *p,size_t n)
{for(int i=0;i<GUARD;i++){assert(p[i]==0xcafe);assert(p[GUARD+n+i]==0xcafe);}}
static void draw(direct_gfx_t *g,int x,int baseline,int size)
{direct_home_text(g,x,baseline,"电网 42.5 W g",size,0xffff);}
static void clipping(int x,int baseline,int size)
{
 fill(full,sizeof(full)/sizeof(full[0]),0xcafe);fill(full+GUARD,W*H,0x0863);
 direct_gfx_t g={.pixels=full+GUARD,.width=W,.height=H,.y0=0};
 draw(&g,x,baseline,size);guards(full,W*H);
 int antialias=0;
 for(int i=0;i<W*H;i++)antialias+=full[GUARD+i]!=0x0863 && full[GUARD+i]!=0xffff;
 assert(antialias>0);
 for(int y=0;y<H;y+=7){
  int h=H-y<7?H-y:7;
  fill(strip,sizeof(strip)/sizeof(strip[0]),0xcafe);fill(strip+GUARD,(size_t)W*h,0x0863);
  g=(direct_gfx_t){.pixels=strip+GUARD,.width=W,.height=h,.y0=y};
  draw(&g,x,baseline,size);guards(strip,(size_t)W*h);
  assert(!memcmp(strip+GUARD,full+GUARD+y*W,(size_t)W*h*sizeof(uint16_t)));
 }
}
static void baseline(void)
{
 uint16_t p[32*32]={0};direct_gfx_t g={.pixels=p,.width=32,.height=32};
 direct_home_text(&g,5,20,"H",12,0xffff);
 int top=32,bottom=-1;
 for(int y=0;y<32;y++)for(int x=0;x<32;x++)if(p[y*32+x]){if(y<top)top=y;if(y>bottom)bottom=y;}
 assert(top==11 && bottom==19); /* Pillow anchor ls: native 12px H is -9..0. */
 memset(p,0,sizeof(p));direct_home_text(&g,5,20,"g",12,0xffff);
 int below=0;for(int y=20;y<24;y++)for(int x=0;x<32;x++)below+=p[y*32+x]!=0;
 assert(below>0);
}
int main(void)
{
 assert(direct_home_text_width("W",12)==11);
 assert(direct_home_text_width("W",14)==13);
 assert(direct_home_text_width("W",16)==14);
 assert(direct_home_text_width("",12)==0 && direct_home_text_width(NULL,12)==0);
 assert(direct_home_text_width("12 W",12)==direct_home_text_width("12 ",12)+direct_home_text_width("W",12));
 const char *words[]={"电网","光伏","电池","负载","充电","放电","待机","数据过期","未更新","未知"};
 for(int s=12;s<=16;s+=2){
  for(unsigned i=0;i<sizeof(words)/sizeof(words[0]);i++)
   assert(direct_home_text_width(words[i],s)==(int)(strlen(words[i])/3)*s);
  assert(direct_home_text_width("☃",s)==direct_home_text_width("?",s));
  clipping(5,30,s);clipping(-8,8,s);clipping(180,63,s);
 }
 baseline();
 direct_home_text(NULL,0,0,"H",12,0xffff);
 uint16_t p=0x1234;direct_gfx_t g={.pixels=&p,.width=1,.height=1};
 direct_home_text(&g,INT_MIN,INT_MAX,"W",12,0xffff);assert(p==0x1234);
 direct_home_text(&g,INT_MAX,INT_MIN,"W",12,0xffff);assert(p==0x1234);
 puts("home font: native sizes, baselines, required glyphs, AA and strip clipping passed");
 return 0;
}
