#include "direct_gfx.h"
#include "direct_gfx_assets.h"
#include <stddef.h>
#include <string.h>

void direct_gfx_begin(direct_gfx_t *g,uint16_t *pixels,int w,int h,int y0,uint16_t bg)
{
 if(!g)return;
 *g=(direct_gfx_t){.pixels=pixels,.width=w,.height=h,.y0=y0,.theme_light=false,.language_zh=false};
 if(!pixels || w<=0 || h<=0)return;
 for(size_t i=0;i<(size_t)w*(size_t)h;i++)pixels[i]=bg;
}
static void pixel(direct_gfx_t *g,int x,int y,uint16_t color)
{
 if(!g || !g->pixels || x<0 || x>=g->width || (int64_t)y<g->y0 || (int64_t)y>=(int64_t)g->y0+g->height)return;
 g->pixels[(size_t)(y-g->y0)*g->width+x]=color;
}
void direct_gfx_rect(direct_gfx_t *g,int x,int y,int w,int h,uint16_t color)
{
 if(!g || !g->pixels || w<=0 || h<=0)return;
 int64_t l=x,r=(int64_t)x+w,t=y,b=(int64_t)y+h;
 if(l<0)l=0;
 if(r>g->width)r=g->width;
 if(t<g->y0)t=g->y0;
 if(b>(int64_t)g->y0+g->height)b=(int64_t)g->y0+g->height;
 for(int64_t row=t;row<b;row++)for(int64_t col=l;col<r;col++)pixel(g,(int)col,(int)row,color);
}
void direct_gfx_line(direct_gfx_t *g,int x0,int y0,int x1,int y1,uint16_t color)
{
 if(!g || !g->pixels || g->width<=0 || g->height<=0)return;
 /* Preserve the original Bresenham phase across strips; clipping endpoints
    before rasterization changes diagonals at strip boundaries. */
 int dx=x1>x0?x1-x0:x0-x1,sx=x0<x1?1:-1;
 int dy=y1>y0?y0-y1:y1-y0,sy=y0<y1?1:-1,err=dx+dy;
 for(;;){pixel(g,x0,y0,color);if(x0==x1 && y0==y1)break;int e=err*2;if(e>=dy){err+=dy;x0+=sx;}if(e<=dx){err+=dx;y0+=sy;}}
}
uint32_t direct_gfx_utf8_next(const char **text)
{
 const unsigned char *p=(const unsigned char *)*text;
 if(!*p)return 0;
 unsigned n;uint32_t cp,min;
 if(*p<0x80){*text+=1;return *p;}
 if(*p>=0xc2 && *p<=0xdf){n=2;cp=*p&31;min=0x80;}
 else if(*p>=0xe0 && *p<=0xef){n=3;cp=*p&15;min=0x800;}
 else if(*p>=0xf0 && *p<=0xf4){n=4;cp=*p&7;min=0x10000;}
 else{*text+=1;return 0xfffd;}
 for(unsigned i=1;i<n;i++){
  if(!p[i] || (p[i]&0xc0)!=0x80){*text+=1;return 0xfffd;}
  cp=(cp<<6)|(p[i]&63);
 }
 *text+=n;
 return cp<min || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff)?0xfffd:cp;
}
/* A translated textual setting can contain letters outside the numeric-unit
   subset. Select one size for its entire run, never mix fallback body glyphs
   into large text. Width calculation and drawing must make the same choice. */
static int text_size(const char *text,int size)
{
 if(size!=22 && size!=24 && size!=28)return 16;
 int height=size==28?28:24;
 const char *p=text;
 while(*p)if(direct_gfx_glyph(direct_gfx_utf8_next(&p),size)->height!=height)return 16;
 return size;
}
int direct_gfx_text_width(const char *text,int size)
{
 int width=0;if(!text)return 0;
 size=text_size(text,size);
 while(*text)width+=direct_gfx_glyph(direct_gfx_utf8_next(&text),size)->advance;
 return width;
}
void direct_gfx_text(direct_gfx_t *g,int x,int y,const char *text,int size,uint16_t color)
{
 if(!g || !g->pixels || !text)return;
 size=text_size(text,size);
 if((int64_t)y+28<=g->y0 || (int64_t)y>=(int64_t)g->y0+g->height)return;
 while(*text){
  const direct_gfx_glyph_t *glyph=direct_gfx_glyph(direct_gfx_utf8_next(&text),size);
  for(unsigned row=0;row<glyph->height;row++)for(unsigned col=0;col<glyph->width;col++){
   int px=x+(int)col,py=y+(int)row;
   if(px<0 || px>=g->width || py<g->y0 || (int64_t)py>=(int64_t)g->y0+g->height)continue;
   unsigned a;
   if(glyph->bpp==4){unsigned packed=glyph->data[row*glyph->stride+col/2];a=(col&1)?(packed&15):(packed>>4);}
   else a=(glyph->data[row*glyph->stride+col/8]&(0x80>>(col%8)))?15:0;
   if(a){
    size_t at=(size_t)(py-g->y0)*g->width+px;
    if(a==15)g->pixels[at]=color;
    else{
     unsigned bg=g->pixels[at],inv=15-a;
     unsigned r=(((color>>11)&31)*a+((bg>>11)&31)*inv+7)/15;
     unsigned green=(((color>>5)&63)*a+((bg>>5)&63)*inv+7)/15;
     unsigned b=((color&31)*a+(bg&31)*inv+7)/15;
     g->pixels[at]=(uint16_t)((r<<11)|(green<<5)|b);
    }
   }
  }
  x+=glyph->advance;if(x>=g->width)break;
 }
}
static void stroke(direct_gfx_t *g,int x,int y,int a,int b,int c,int d,uint16_t color)
{direct_gfx_line(g,x+a,y+b,x+c,y+d,color);}
static void box(direct_gfx_t *g,int x,int y,int l,int t,int r,int b,uint16_t color)
{stroke(g,x,y,l,t,r,t,color);stroke(g,x,y,r,t,r,b,color);stroke(g,x,y,r,b,l,b,color);stroke(g,x,y,l,b,l,t,color);}
void direct_gfx_icon(direct_gfx_t *g,direct_icon_t icon,int x,int y,uint16_t color)
{
 switch(icon){
 case DIRECT_ICON_GRID:
  stroke(g,x,y,5,30,16,2,color);stroke(g,x,y,16,2,27,30,color);stroke(g,x,y,16,2,16,30,color);
  stroke(g,x,y,3,10,29,10,color);stroke(g,x,y,7,5,25,5,color);stroke(g,x,y,8,16,24,16,color);stroke(g,x,y,6,23,26,23,color);stroke(g,x,y,3,30,29,30,color);
  stroke(g,x,y,10,16,24,23,color);stroke(g,x,y,22,16,8,23,color);stroke(g,x,y,8,23,25,30,color);stroke(g,x,y,24,23,7,30,color);break;
 case DIRECT_ICON_SOLAR:
  stroke(g,x,y,6,12,26,12,color);stroke(g,x,y,26,12,31,27,color);stroke(g,x,y,31,27,1,27,color);stroke(g,x,y,1,27,6,12,color);
  stroke(g,x,y,12,12,10,27,color);stroke(g,x,y,20,12,22,27,color);stroke(g,x,y,3,20,29,20,color);stroke(g,x,y,16,27,16,31,color);stroke(g,x,y,10,31,22,31,color);
  box(g,x,y,24,2,28,6,color);stroke(g,x,y,26,0,26,1,color);stroke(g,x,y,21,4,23,4,color);stroke(g,x,y,29,4,31,4,color);break;
 case DIRECT_ICON_INVERTER:
  box(g,x,y,6,1,26,31,color);box(g,x,y,9,5,23,14,color);stroke(g,x,y,10,10,13,7,color);stroke(g,x,y,13,7,18,12,color);stroke(g,x,y,18,12,22,8,color);
  for(int i=19;i<=25;i+=3)stroke(g,x,y,10,i,22,i,color);
  stroke(g,x,y,10,28,12,28,color);break;
 case DIRECT_ICON_LOAD:
  stroke(g,x,y,1,14,16,2,color);stroke(g,x,y,16,2,31,14,color);stroke(g,x,y,5,11,5,30,color);stroke(g,x,y,27,11,27,30,color);stroke(g,x,y,5,30,27,30,color);
  box(g,x,y,13,20,19,30,color);box(g,x,y,8,16,11,20,color);box(g,x,y,22,16,25,20,color);break;
 case DIRECT_ICON_BATTERY:
  box(g,x,y,1,7,27,25,color);box(g,x,y,28,12,31,20,color);for(int i=5;i<=21;i+=8)box(g,x,y,i,11,i+4,21,color);break;
 case DIRECT_ICON_GENERATOR:
  box(g,x,y,2,9,30,28,color);stroke(g,x,y,6,9,6,3,color);stroke(g,x,y,6,3,24,3,color);stroke(g,x,y,24,3,28,9,color);box(g,x,y,6,13,13,18,color);
  stroke(g,x,y,21,12,17,20,color);stroke(g,x,y,17,20,23,20,color);stroke(g,x,y,23,20,19,26,color);stroke(g,x,y,6,29,6,31,color);stroke(g,x,y,26,29,26,31,color);break;
 case DIRECT_ICON_WARNING:
  stroke(g,x,y,16,2,1,29,color);stroke(g,x,y,1,29,31,29,color);stroke(g,x,y,31,29,16,2,color);stroke(g,x,y,16,11,16,20,color);direct_gfx_rect(g,x+15,y+24,3,3,color);break;
 default:break;
 }
}
