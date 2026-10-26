#include "direct_home_asset.h"
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { W=352, H=224, GUARD=32 };
static uint16_t full[W*H+2*GUARD], strip[W*17+2*GUARD];
static void fill(uint16_t *p, size_t n, uint16_t c)
{ for(size_t i=0;i<n;i++)p[i]=c; }
static void guards(const uint16_t *p,size_t n)
{ for(int i=0;i<GUARD;i++){assert(p[i]==0xcafe);assert(p[GUARD+n+i]==0xcafe);} }
static void check(int x,int y,uint16_t bg,uint32_t expected)
{
 fill(full,sizeof(full)/sizeof(full[0]),0xcafe);
 fill(full+GUARD,W*H,bg);
 direct_gfx_t g={.pixels=full+GUARD,.width=W,.height=H,.y0=0};
 direct_home_asset_draw(&g,x,y,bg);
 guards(full,W*H);
 int changed=0;
 for(int i=0;i<W*H;i++)changed+=full[GUARD+i]!=bg;
 assert(changed>10000);
 /* FNV of pixels rendered by the original uncompressed C asset, low byte first.
  * This checks the actual decoder and alpha blending, independently of packing. */
 uint32_t hash=2166136261U;
 for(unsigned i=0;i<W*H;i++){
  hash^=full[GUARD+i]&255;hash*=16777619U;
  hash^=full[GUARD+i]>>8;hash*=16777619U;
 }
 assert(hash==expected);
 /* Exterior transparent pixels and space outside the source remain untouched. */
 assert(full[GUARD]==bg);
 assert(full[GUARD+W*H-1]==bg);
 for(int step=16;step<=17;step++)
  for(int top=0;top<H;top+=step){
   int h=H-top<step?H-top:step;
   fill(strip,sizeof(strip)/sizeof(strip[0]),0xcafe);
   fill(strip+GUARD,(size_t)W*h,bg);
   g=(direct_gfx_t){.pixels=strip+GUARD,.width=W,.height=h,.y0=top};
   direct_home_asset_draw(&g,x,y,bg);
   guards(strip,(size_t)W*h);
   assert(!memcmp(strip+GUARD,full+GUARD+top*W,(size_t)W*h*sizeof(uint16_t)));
  }
}
int main(void)
{
 assert(DIRECT_HOME_ASSET_WIDTH==320 && DIRECT_HOME_ASSET_HEIGHT==192);
 check(16,16,0x0863,0x2f9d963c);check(16,16,0xffff,0xe2078c05);check(-23,-7,0x0863,0x1d602db6);
 direct_home_asset_draw(NULL,0,0,0);
 direct_gfx_t g={0};direct_home_asset_draw(&g,0,0,0);
 uint16_t p=0x1234;g=(direct_gfx_t){.pixels=&p,.width=1,.height=1,.y0=0};
 /* The source top-left pixel is transparent: do not erase existing content. */
 direct_home_asset_draw(&g,0,0,0);assert(p==0x1234);
 direct_home_asset_draw(&g,INT_MAX,INT_MIN,0);assert(p==0x1234);
 direct_home_asset_draw(&g,INT_MIN,INT_MAX,0);assert(p==0x1234);
 puts("home asset: clipped strips, transparent backgrounds and guards passed");
 return 0;
}
