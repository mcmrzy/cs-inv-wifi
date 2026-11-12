/* Only CS-MB-PANEL local scenes. Inverter screens live in panel_ui. */
#include "direct_ui_scene.h"
#include "direct_home.h"
#include "direct_home_asset.h"
#include "direct_home_font.h"
#include "full_ui_bridge.h"
#include "panel_theme.h"
#include "panel_display_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include "direct_ui_locale.h"
#include <string.h>
#define BG (panel_theme(g->theme_light).background)
#define FG (panel_theme(g->theme_light).text)
#define MUTED (panel_theme(g->theme_light).muted)
#define LINE (panel_theme(g->theme_light).line)
#define GREEN (panel_theme(g->theme_light).accent)
#define GOLD (panel_theme(g->theme_light).warning)
#define ORANGE GOLD
#define SELECT_BG (panel_theme(g->theme_light).selected)
#define SURFACE (panel_theme(g->theme_light).surface)
/* Translate before measuring. Width limits remove whole UTF-8 codepoints. */
static const char*localized(direct_gfx_t*g,const char*s,char out[512]){
 const char*result=direct_ui_translate(s,g->language_zh,out,512);
 if(result!=out){snprintf(out,512,"%s",result?result:"");}return out;
}
static void fit(char*s,int size,int width){
 while(*s && direct_gfx_text_width(s,size)>width){
  size_t n=strlen(s);do{n--;}while(n && ((unsigned char)s[n]&0xc0)==0x80);s[n]=0;
 }
}
static bool visible(direct_gfx_t*g,int y,int height){return y+height>g->y0 && y<g->y0+g->height;}
static void t(direct_gfx_t*g,int x,int y,const char*s,int size,uint16_t color){
 if(!visible(g,y,28))return;
 char text[512];localized(g,s,text);fit(text,size,308-x);direct_gfx_text(g,x,y,text,size,color);
}
static void right(direct_gfx_t*g,int x,int y,const char*s,int size,uint16_t color){
 if(!visible(g,y,28))return;
 char text[512];localized(g,s,text);fit(text,size,x-12);
 direct_gfx_text(g,x-direct_gfx_text_width(text,size),y,text,size,color);
}
static void center(direct_gfx_t*g,int x,int y,const char*s,int size,uint16_t color){
 if(!visible(g,y,28))return;
 char text[512];localized(g,s,text);fit(text,size,296);
 int width=direct_gfx_text_width(text,size),left=x-width/2;
 if(left<12)left=12;
 if(left+width>308)left=308-width;
 direct_gfx_text(g,left,y,text,size,color);
}
static void rule(direct_gfx_t*g,int y){direct_gfx_rect(g,12,y,296,1,LINE);}
static void title(direct_gfx_t*g,const char*s,const char*info){
 direct_gfx_rect(g,0,0,320,27,SURFACE);
 direct_gfx_rect(g,10,9,3,12,GREEN);
 if(visible(g,6,24)){
  char text[512];localized(g,s,text);fit(text,14,info&&*info?216:296);
  direct_gfx_text(g,19,6,text,14,FG);if(info)right(g,308,7,info,12,GREEN);
 }
 rule(g,27);
}
static void footer(direct_gfx_t*g,const char*a,const char*b,const char*note){
 rule(g,220);
 if(!visible(g,221,20))return;
 if(note && *note)t(g,12,221,note,12,GOLD);
 else {
  char left[512],righttext[512];localized(g,a,left);localized(g,b?b:"",righttext);
  int rw=direct_gfx_text_width(righttext,12);fit(left,12,284-rw);
  direct_gfx_text(g,12,221,left,12,MUTED);if(b)right(g,308,221,b,12,MUTED);
 }
}
static void physical_footer(direct_gfx_t*g,const direct_ui_state_t*u,const direct_ui_data_t*d){
 if(u->message[0])return; /* retain save/error notices until the next key */
 const char *hints[6];direct_ui_key_hints(u,d,hints);
 direct_gfx_rect(g,0,220,320,20,SURFACE);direct_gfx_rect(g,0,220,320,1,LINE);
 if(!visible(g,221,20))return;
 for(unsigned i=0;i<6;i++){
  int left=(int)(i*320U/6U),rightedge=(int)((i+1)*320U/6U);
  char text[512];localized(g,hints[i],text);fit(text,12,rightedge-left-4);
  if(!g->language_zh){
   const char *shortlabel=!strcmp(hints[i],"Confirm")?"OK":!strcmp(hints[i],"Review")?"Check":
    !strcmp(hints[i],"Cancel")?(u->page==UI_UPGRADE?"Stop":"Undo"):!strcmp(hints[i],"Details")?"Detail":
    !strcmp(hints[i],"Summary")?"Info":!strcmp(hints[i],"Overview")?"Home":!strcmp(hints[i],"Change")?"Set":NULL;
   if(shortlabel)snprintf(text,sizeof(text),"%s",shortlabel);
  }
  int width=direct_gfx_text_width(text,12);
  direct_gfx_text(g,left+(rightedge-left-width)/2,221,text,12,!strcmp(hints[i],"--")?MUTED:i==5?GREEN:FG);
  if(i)direct_gfx_rect(g,left,223,1,15,LINE);
 }
}
/* Home positions are native LCD pixels and text coordinates are baselines. */
#define HOME_ASSET_Y 32 /* Header height; energy routes are relative to the asset. */
typedef struct {int16_t x,y;} home_point_t;
static void home_stroke(direct_gfx_t*g,int x,int y,int x1,int y1,int width,uint16_t color){
 int radius=width/2,top=y<y1?y:y1,bottom=y>y1?y:y1;
 if(!visible(g,top-radius,bottom-top+width))return;
 if(y==y1)direct_gfx_rect(g,(x<x1?x:x1)-radius,y-radius,abs(x1-x)+width,width,color);
 else direct_gfx_rect(g,x-radius,top-radius,width,bottom-top+width,color);
}
static uint16_t home_tint(uint16_t base,uint16_t light,unsigned amount){
 unsigned r=((base>>11)*(4-amount)+(light>>11)*amount)/4;
 unsigned g=(((base>>5)&63)*(4-amount)+((light>>5)&63)*amount)/4;
 unsigned b=((base&31)*(4-amount)+(light&31)*amount)/4;
 return (uint16_t)((r<<11)|(g<<5)|b);
}
static void home_cable(direct_gfx_t*g,const home_point_t *p,unsigned n,uint16_t color,
                       bool active,bool animate,bool reverse,uint8_t phase){
 uint16_t rail=active?home_tint(0x2104,color,3):g->theme_light?0x8c71:0x6b6d;
 unsigned total=0;
 for(unsigned i=0;i+1<n;i++){
  total+=(unsigned)(abs(p[i+1].x-p[i].x)+abs(p[i+1].y-p[i].y));
  home_stroke(g,p[i].x,p[i].y+HOME_ASSET_Y,p[i+1].x,p[i+1].y+HOME_ASSET_Y,active?5:3,0x2104);
 }
 for(unsigned i=0;i+1<n;i++)
  home_stroke(g,p[i].x,p[i].y+HOME_ASSET_Y,p[i+1].x,p[i+1].y+HOME_ASSET_Y,active?3:1,rail);
 if(!active || !animate || !total)return;
 /* Draw short pulses on the rail. Distance is continuous through a corner,
  * and advances two pixels per frame regardless of the length of the route. */
 const uint16_t head=home_tint(color,0xffff,3),tail=home_tint(color,0xffff,1);
 const unsigned motion=2U*phase;
 unsigned travelled=0;
 for(unsigned i=0;i+1<n;i++){
  int dx=p[i+1].x-p[i].x,dy=p[i+1].y-p[i].y;
  int sx=(dx>0)-(dx<0),sy=(dy>0)-(dy<0);
  unsigned length=(unsigned)(abs(dx)+abs(dy));
  int top=(dy<0?p[i+1].y:p[i].y)+HOME_ASSET_Y;
  if(!visible(g,top-1,abs(dy)+3)){travelled+=length;continue;}
  for(unsigned step=0;step<length;step++){
   unsigned distance=travelled+step;
   unsigned offset=(motion-(reverse?total-distance:distance))&31U;
   if(offset>8)continue;
   int x=p[i].x+sx*(int)step,y=p[i].y+HOME_ASSET_Y+sy*(int)step;
   bool core=offset>=1 && offset<=5;
   direct_gfx_rect(g,x-(core && sy),y-(core && sx),core && sy?3:1,core && sx?3:1,
                   offset<4?head:offset<7?tail:color);
  }
  travelled+=length;
 }
}
static void home_wifi(direct_gfx_t*g,const direct_ui_data_t*d,uint16_t fg,uint16_t muted){
 int level=direct_home_wifi_level(d);uint16_t dim=LINE;
 const home_point_t arcs[3][5]={{{304,16},{305,15},{306,15},{307,15},{308,16}},
  {{301,13},{303,12},{306,11},{309,12},{311,13}},
  {{298,10},{302,8},{306,8},{310,8},{314,10}}};
 for(unsigned a=0;a<3;a++)for(unsigned i=0;i<4;i++)
  direct_gfx_line(g,arcs[a][i].x,arcs[a][i].y,arcs[a][i+1].x,arcs[a][i+1].y,level>(int)a?fg:dim);
 direct_gfx_rect(g,305,18,2,2,level==-1?muted:fg);
 if(level==-1)direct_gfx_line(g,298,7,314,21,muted);
}
/* Names are 14px bold in the foreground colour of the active theme; the light
 * theme uses dark text on the light background instead of a solid name plate. */
static void home_name(direct_gfx_t*g,int x,int baseline,const char*zh,const char*en,uint16_t fg){
 direct_home_text(g,x,baseline,g->language_zh?zh:en,14,fg);
}
static void home_horizontal(direct_gfx_t*g,int baseline,const char*zh,const char*en,const char*powertext,const char*voltage,uint16_t fg,uint16_t muted){
 home_name(g,166,baseline,zh,en,fg);
 /* Compact boundary values can use native 12px within the same fixed column. */
 int size=direct_home_text_width(powertext,16)>58?12:16;
 direct_home_text(g,204,baseline,powertext,size,fg);
 direct_home_text(g,264,baseline,voltage,12,muted);
}
static void home(direct_gfx_t*g,const direct_ui_data_t*d){
 const RunParamDef*p=&d->run;
 uint16_t bg=BG,fg=FG,muted=MUTED;
 direct_gfx_rect(g,0,0,320,220,bg);
 direct_gfx_rect(g,0,0,320,24,bg);
 t(g,10,4,"Energy flow",14,fg);
 char clock[16]="--:--";if(d->clock_valid)snprintf(clock,sizeof(clock),"%02d:%02d",d->hour%24,d->minute%60);
 direct_home_text(g,282-direct_home_text_width(clock,12),18,clock,12,muted);
 home_wifi(g,d,fg,muted);
 direct_home_asset_draw(g,0,HOME_ASSET_Y,bg);
 static const home_point_t pv[]={{170,65},{137,65},{137,88}};
 static const home_point_t grid[]={{56,83},{56,105},{123,105}};
 static const home_point_t battery[]={{133,116},{133,141}};
 static const home_point_t load[]={{145,109},{222,109},{222,144}};
 direct_home_values_t v;direct_home_values(d,&v);
 bool live=d->dsp_online,animate=live&&!d->ota_busy;
 home_cable(g,pv,3,PANEL_RGB(240,197,111),live&&p->Ppv,animate,false,d->home_flow_phase+4);
 home_cable(g,grid,3,PANEL_RGB(120,192,185),live&&p->ACInWatt,animate,p->ACInWatt<0,d->home_flow_phase);
 home_cable(g,battery,2,PANEL_RGB(139,214,157),live&&v.battery_watts,animate,v.battery_state==DIRECT_HOME_DISCHARGING,d->home_flow_phase+8);
 home_cable(g,load,3,PANEL_RGB(230,153,99),live&&p->OutputWatt,animate,p->OutputWatt<0,d->home_flow_phase+12);
 home_name(g,8,39,"电网","Grid",fg);
 direct_home_text(g,8,56,v.grid_power,16,fg);direct_home_text(g,8,68,v.grid_voltage,12,muted);
 home_horizontal(g,39,"光伏","PV",v.pv_power,v.pv_voltage,fg,muted);
 home_name(g,8,173,"电池","Battery",fg);
 direct_home_text(g,8,195,v.battery_power,16,fg);
 const char *zh="未知",*en="Unknown";
 switch(v.battery_state){
  case DIRECT_HOME_CHARGING:zh="充电";en="Chg";break;
  case DIRECT_HOME_DISCHARGING:zh="放电";en="Dchg";break;
  case DIRECT_HOME_IDLE:zh="待机";en="Idle";break;
  default:break;
 }
 direct_home_text(g,8+direct_home_text_width(v.battery_power,16)+8,195,g->language_zh?zh:en,12,muted);
 direct_home_text(g,8,217,v.battery_voltage,12,muted);
 direct_home_text(g,49,217,v.battery_soc,12,muted);
 home_horizontal(g,217,"负载","Load",v.load_power,v.load_voltage,fg,muted);
 direct_gfx_rect(g,120,1,116,22,BG);
 t(g,124,4,d->dsp_online?"Live data":d->dsp_display_held?"Data stale":"No live data",12,d->dsp_online?GREEN:ORANGE);
}
static void menu_mark(direct_gfx_t *g,unsigned icon,int y,uint16_t ink){
 const int x=25;
 switch(icon){
 case 0:
  direct_gfx_line(g,x,y+11,x+5,y+6,ink);
  direct_gfx_line(g,x+5,y+6,x+9,y+9,ink);
  direct_gfx_line(g,x+9,y+9,x+15,y+2,ink);
  break;
 case 1:
  for(int i=0;i<3;i++){
   int top=y+2+i*5;
   direct_gfx_rect(g,x,top,15,1,ink);
   direct_gfx_rect(g,x+(i%2?9:4),top-1,3,3,ink);
  }
  break;
 case 2:
  direct_gfx_rect(g,x+7,y,2,8,ink);
  direct_gfx_line(g,x+4,y+3,x+1,y+7,ink);
  direct_gfx_line(g,x+1,y+7,x+4,y+13,ink);
  direct_gfx_line(g,x+4,y+13,x+11,y+13,ink);
  direct_gfx_line(g,x+11,y+13,x+14,y+7,ink);
  direct_gfx_line(g,x+14,y+7,x+11,y+3,ink);
  break;
 case 3:
  direct_gfx_line(g,x+7,y+1,x,y+14,ink);
  direct_gfx_line(g,x+7,y+1,x+14,y+14,ink);
  direct_gfx_rect(g,x,y+14,15,1,ink);
  direct_gfx_rect(g,x+7,y+6,1,4,ink);
  direct_gfx_rect(g,x+7,y+12,1,1,ink);
  break;
 default:
  direct_gfx_rect(g,x,y+1,15,1,ink);
  direct_gfx_rect(g,x,y+10,15,1,ink);
  direct_gfx_rect(g,x,y+2,1,8,ink);
  direct_gfx_rect(g,x+14,y+2,1,8,ink);
  direct_gfx_rect(g,x+7,y+11,1,3,ink);
  direct_gfx_rect(g,x+4,y+14,7,1,ink);
  break;
 }
}
static void nav_row(direct_gfx_t *g,int y,int height,const char *label,bool selected,int icon,uint8_t focus){
 panel_card(g,10,y,300,height,selected?SELECT_BG:SURFACE);
 if(selected)direct_gfx_rect(g,12,y+5,2+focus,height-10,GREEN);
 int text_y=y+(height-16)/2;
 if(icon>=0)menu_mark(g,(unsigned)icon,text_y,selected?GREEN:MUTED);
 t(g,icon>=0?52:25,text_y,label,16,selected?GREEN:FG);
 right(g,296,text_y,">",16,selected?GREEN:MUTED);
}
static void menu(direct_gfx_t*g,const direct_ui_state_t*u,uint8_t focus){
 title(g,"System menu",NULL);
 for(unsigned i=0;i<direct_ui_menu_count();i++)
  nav_row(g,34+(int)i*36,32,direct_ui_menu_name(i),i==u->selected,(int)i,focus);
 footer(g,"UP/DN Select","ENTER Open",u->message);
}
static void submenu(direct_gfx_t*g,const direct_ui_state_t*u,uint8_t focus){
 unsigned count=direct_ui_submenu_count(u->menu_selected),start=u->selected/DIRECT_UI_SUBMENU_ROWS*DIRECT_UI_SUBMENU_ROWS;
 char info[20];snprintf(info,sizeof(info),"%u / %u",start/DIRECT_UI_SUBMENU_ROWS+1,(count+DIRECT_UI_SUBMENU_ROWS-1)/DIRECT_UI_SUBMENU_ROWS);
 title(g,direct_ui_menu_name(u->menu_selected),info);
 for(unsigned i=0;i<DIRECT_UI_SUBMENU_ROWS&&start+i<count;i++){
  nav_row(g,33+(int)i*31,28,direct_ui_submenu_name(u->menu_selected,start+i),start+i==u->selected,-1,focus);
 }
 footer(g,"UP/DN Select","ENTER Open",u->message);
}
static void lcd_test(direct_gfx_t*g,bool inverted){
 title(g,"LCD contrast test",inverted?"B: INV ON":"A: INV OFF");
 t(g,12,33,"Gray ramp: black to white",12,FG);
 for(unsigned i=0;i<8;i++){
  unsigned r=(i*31+3)/7,b=r,green=(i*63+3)/7;
  uint16_t c=(uint16_t)((r<<11)|(green<<5)|b);
  direct_gfx_rect(g,12+(int)i*37,49,35,23,c);
 }
 const uint16_t colors[]={0xf800,0x07e0,0x001f};
 for(unsigned i=0;i<3;i++)direct_gfx_rect(g,12+(int)i*100,84,96,22,colors[i]);
 t(g,12,117,"0123456789 52.4 V",22,FG);
 direct_gfx_rect(g,12,146,296,25,FG);
 center(g,160,151,"Black text on white",14,BG);
 t(g,12,181,"PV",14,GOLD);t(g,76,181,"GRID",14,GREEN);
 t(g,159,181,"BAT",14,GREEN);t(g,239,181,"LOAD",14,ORANGE);
 t(g,12,202,"Compare same angle in A and B",12,FG);
 footer(g,"L/R Info, A, B","ESC Exit",NULL);
}
static void upgrade(direct_gfx_t*g,const direct_ui_data_t*d,const direct_ui_view_t*v){
 title(g,"Firmware upgrade",NULL);
 center(g,160,38,v->rows[0],16,d->ota.state==OTA_STATE_FAILED?ORANGE:GREEN);
 /* Reserve independent widths for source/target and version. A long version
  * must not overwrite the connection label. */
 if(visible(g,65,28)){
  char source[512],version[512];localized(g,v->rows[1],source);localized(g,v->rows[3],version);
  fit(source,14,180);fit(version,14,100);
  direct_gfx_text(g,16,65,source,14,GREEN);
  direct_gfx_text(g,304-direct_gfx_text_width(version,14),65,version,14,FG);
 }
 direct_gfx_rect(g,16,94,288,12,SELECT_BG);
 direct_gfx_rect(g,16,94,(int)(288U*(d->ota.progress>100?100:d->ota.progress)/100U),12,GREEN);
 center(g,160,115,v->rows[2],16,FG);
 t(g,16,145,v->rows[4],14,d->ota.state==OTA_STATE_FAILED?ORANGE:MUTED);
 t(g,16,171,v->rows[6],12,FG);
 t(g,16,197,v->rows[5],12,ORANGE);
 footer(g,v->footer,"",NULL);
}
static void appearance(direct_gfx_t *g,const direct_ui_state_t *u,const direct_ui_data_t *d,const direct_ui_view_t *v){
    g->theme_light=d->theme_light;g->language_zh=d->language_zh;
    direct_gfx_rect(g,0,0,320,240,BG);title(g,"Appearance",NULL);
    for(unsigned i=0;i<PANEL_APPEARANCE_ROWS;i++){
        int y=38+(int)i*24;bool selected=u->selected==i;
        panel_card(g,10,y-3,300,23,selected?SELECT_BG:SURFACE);
        if(selected)direct_gfx_rect(g,12,y,3,16,GREEN);
        char label[64];const char *colon=strchr(v->rows[i],':');
        if(colon){
            size_t n=(size_t)(colon-v->rows[i]);if(n>=sizeof(label))n=sizeof(label)-1;
            memcpy(label,v->rows[i],n);label[n]=0;
            const char *value=colon+1;while(*value==' ')value++;
            t(g,24,y,label,14,FG);right(g,296,y,value,14,selected?GREEN:MUTED);
        }else{
            t(g,24,y,v->rows[i],14,FG);right(g,296,y,"ENTER Confirm",14,selected?GREEN:MUTED);
        }
    }
    footer(g,"UP/DN Select","ESC Cancel",u->message);physical_footer(g,u,d);
}
static void info(direct_gfx_t *g,const direct_ui_state_t *u,const direct_ui_view_t *v){
 title(g,v->title,NULL);
 for(unsigned row=0;row<DIRECT_UI_ROWS;row++){
  int y=35+(int)row*20;bool selected=v->selected==(int)row;
  if(selected)panel_card(g,10,y-1,300,19,SELECT_BG);
  const char *text=v->rows[row],*colon=strchr(text,':');if(!*text)continue;
  if(colon){
   char label[48];size_t n=(size_t)(colon-text);if(n>=sizeof(label))n=sizeof(label)-1;
   memcpy(label,text,n);label[n]=0;const char *value=colon+1;while(*value==' ')value++;
   char left[512],righttext[512];localized(g,label,left);localized(g,value,righttext);
   if(direct_gfx_text_width(left,12)+direct_gfx_text_width(righttext,12)<275){
    t(g,16,y,label,12,MUTED);right(g,304,y,value,12,selected?GREEN:FG);
   }else t(g,16,y,text,12,selected?GREEN:FG);
  }else t(g,16,y,text,12,selected?GREEN:FG);
 }
 footer(g,u->page==UI_TIME?"L/R Edit":"", "ESC Back",u->message);
}
static void factory(direct_gfx_t *g,const direct_ui_state_t *u,const direct_ui_view_t *v){
 title(g,v->title,NULL);direct_gfx_icon(g,DIRECT_ICON_WARNING,144,46,GOLD);
 center(g,160,94,v->rows[1],14,FG);center(g,160,120,"Inverter settings retained",12,MUTED);
 panel_card(g,65,154,84,36,u->confirm?SURFACE:SELECT_BG);
 panel_card(g,172,154,84,36,u->confirm?SELECT_BG:SURFACE);
 center(g,107,163,"NO",14,u->confirm?MUTED:GREEN);center(g,214,163,"YES",14,u->confirm?GREEN:MUTED);
 footer(g,"L/R Choose","ENTER Confirm",u->message);
}
void full_ui_scene_draw_frame(direct_gfx_t *g,const direct_ui_state_t *u,const direct_ui_data_t *d,const direct_ui_view_t *v,uint8_t focus){
 g->theme_light=d->theme_light;g->language_zh=d->language_zh;direct_gfx_rect(g,0,0,320,240,BG);
 switch(u->page){
 case UI_HOME:home(g,d);if(u->message[0])footer(g,NULL,NULL,u->message);break;
 case UI_MENU:menu(g,u,focus);break;
 case UI_SUBMENU:submenu(g,u,focus);break;
 case UI_APPEARANCE:appearance(g,u,d,v);break;
 case UI_HELP:if(u->detail_page)lcd_test(g,u->detail_page==2);else info(g,u,v);break;
 case UI_FACTORY:factory(g,u,v);break;
 case UI_UPGRADE:upgrade(g,d,v);break;
 case UI_TIME:case UI_PRODUCTION:info(g,u,v);break;
 default:title(g,"Information",NULL);center(g,160,100,"Open inverter functions from menu",14,MUTED);break;
 }
 physical_footer(g,u,d);
}
void direct_ui_scene_draw(direct_gfx_t *g,const direct_ui_state_t *u,const direct_ui_data_t *d,const direct_ui_view_t *v){
 full_ui_scene_draw_frame(g,u,d,v,0);
}
