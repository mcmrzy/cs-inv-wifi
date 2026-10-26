#include "direct_gfx.h"
#include "direct_gfx_assets.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void scene(direct_gfx_t *g)
{
 direct_gfx_rect(g,-8,-4,25,23,0x001f);
 direct_gfx_rect(g,301,230,40,30,0x07e0);
 direct_gfx_line(g,-10,-9,333,241,0xf800);
 direct_gfx_line(g,300,-10,-10,220,0x07e0);
 direct_gfx_text(g,0,13,"Solar PV 234.5W",14,0xffff);
 direct_gfx_text(g,-5,43,"0123456789",28,0xffe0);
 direct_gfx_text(g,15,81,"ASCII !@#%^&*()",12,0xffff);
 direct_gfx_text(g,4,117,"52.3V",22,0xffff);
 direct_gfx_text(g,4,140,"\xe5\xb8\x82\xe7\x94\xb5 \xe5\x85\x89\xe4\xbc\x8f \xe7\x94\xb5\xe6\xb1\xa0",14,0xffff);
 for(int i=0;i<7;i++)direct_gfx_icon(g,(direct_icon_t)i,5+i*44,160,0xffff);
}
int main(void)
{
 static uint16_t full[320*240],stitched[320*240],guard[320*16+2];
 direct_gfx_t g;direct_gfx_begin(&g,full,320,240,0,0);scene(&g);
 for(int y=0;y<240;y+=16){
  guard[0]=0x1234;guard[320*16+1]=0xabcd;
  direct_gfx_begin(&g,guard+1,320,16,y,0);scene(&g);
  assert(guard[0]==0x1234 && guard[320*16+1]==0xabcd);
  memcpy(stitched+y*320,guard+1,320*16*2);
 }
 assert(memcmp(full,stitched,sizeof(full))==0);
 /* Regression: small labels must never select 14px Latin beside 16px CJK;
    units and ordinary letters must belong to the same big text table. */
 assert(direct_gfx_glyph('V',12)==direct_gfx_glyph('V',14));
 assert(direct_gfx_glyph(0x5e02,12)==direct_gfx_glyph(0x5e02,14));
 assert(direct_gfx_text_width("English 123",28)==direct_gfx_text_width("English 123",16));
 assert(direct_gfx_text_width("\xe5\xb8\x82\xe7\x94\xb5 123",28)==direct_gfx_text_width("\xe5\xb8\x82\xe7\x94\xb5 123",16));
 for(int sz=22;sz<=28;sz+=6){
  assert(direct_gfx_glyph('V',sz)->height==direct_gfx_glyph('0',sz)->height);
  assert(direct_gfx_glyph('W',sz)->height==direct_gfx_glyph('0',sz)->height);
 }

 for(int sz=12;sz<=28;sz+=(sz==12?2:sz==14?8:6))for(int c=32;c<=126;c++){
  const direct_gfx_glyph_t *a=direct_gfx_glyph(c,sz);
  int large=(sz==22 || sz==28) && strchr("0123456789 .-+%VWAmkHzhd",c)!=NULL;
  int height=large?(sz==28?28:24):20;
  assert(a && a->height==height && a->bpp==4 && a->stride==(a->width+1)/2);
  assert(a->advance>0 && a->width>0);
 }
 assert(direct_gfx_glyph('W',14)->advance>direct_gfx_glyph('i',14)->advance);
 for(int sz=12;sz<=28;sz+=(sz==12?2:sz==14?8:6))
  for(int c='0';c<='9';c++)assert(direct_gfx_glyph(c,sz)->advance==direct_gfx_glyph('0',sz)->advance);
 assert(direct_gfx_text_width("111",22)==direct_gfx_text_width("000",22));
 assert(direct_gfx_text_width("",14)==0);
 assert(direct_gfx_glyph('0',22)->advance>direct_gfx_glyph('0',14)->advance);
 assert(direct_gfx_glyph(255,14)==direct_gfx_glyph('?',14));
 assert(direct_gfx_has_glyph(0x5e02));
 assert(direct_gfx_glyph(0x5e02,14)->width==16);
 assert(direct_gfx_text_width("\xe5\xb8\x82\xe7\x94\xb5",14)==32);
 /* one absent Unicode scalar plus the '?' fallback, so the width is the
    fallback advance whatever that is for the active Latin font. */
 assert(direct_gfx_text_width("\xf0\x9f\x98\x80",14)==direct_gfx_glyph('?',14)->advance);
 const char *utf="\xe5\xb8\x82\xe7\x94\xb5";
 assert(direct_gfx_utf8_next(&utf)==0x5e02 && direct_gfx_utf8_next(&utf)==0x7535 && !*utf);
 utf="\xed\xa0\x80";assert(direct_gfx_utf8_next(&utf)==0xfffd && !*utf);
 /* Coverage must blend against each existing RGB565 pixel, never flatten
    the whole band. This also checks non-black backgrounds. */
 static uint16_t coverage[320*32];direct_gfx_begin(&g,coverage,320,32,0,0x1234);
 assert(!g.theme_light && !g.language_zh);
 direct_gfx_text(&g,0,0,"8",28,0xabcd);
 const direct_gfx_glyph_t *eight=direct_gfx_glyph('8',28);unsigned partial=0;
 for(unsigned y=0;y<32;y++)for(unsigned x=0;x<320;x++){
  unsigned a=0;
  if(y<eight->height && x<eight->width){unsigned p=eight->data[y*eight->stride+x/2];a=(x&1)?p&15:p>>4;}
  unsigned inv=15-a,bg=0x1234,fg=0xabcd;
  unsigned r=(((fg>>11)&31)*a+((bg>>11)&31)*inv+7)/15;
  unsigned green=(((fg>>5)&63)*a+((bg>>5)&63)*inv+7)/15;
  unsigned b=((fg&31)*a+(bg&31)*inv+7)/15;
  assert(coverage[y*320+x]==(uint16_t)((r<<11)|(green<<5)|b));
  if(a && a<15)partial++;
 }
 assert(partial>0);
 unsigned lit=0;for(unsigned i=0;i<320*240;i++)if(full[i])lit++;
 assert(lit>2000);
 FILE *f=fopen("tests/gfx_fixture.ppm","wb");assert(f);fprintf(f,"P6\n320 240\n255\n");
 for(unsigned i=0;i<320*240;i++){uint16_t p=full[i];unsigned char rgb[]={((p>>11)&31)*255/31,((p>>5)&63)*255/63,(p&31)*255/31};fwrite(rgb,1,3,f);}fclose(f);
 puts("PASS: clipping, 15-strip equality, native ASCII/CJK, UTF8 scalar fallback, 4bpp RGB565 blending and fixed-width digits");
 return 0;
}
