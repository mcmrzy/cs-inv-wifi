#include "direct_ui_scene.h"
#include "direct_home.h"
#include "direct_home_asset.h"
#include "direct_home_font.h"
#include <stdio.h>
#include "direct_ui_locale.h"
#include <string.h>
#define BG (g->theme_light?0xffff:0x0000)
#define FG (g->theme_light?0x0000:0xffff)
#define MUTED (g->theme_light?0x2104:0xdedb)
#define DIM (g->theme_light?0x630c:0x9cf3)
#define LINE 0x8410
#define GREEN (g->theme_light?0x03c7:0x07e0)
#define GOLD (g->theme_light?0x8b40:0xff20)
#define BLUE (g->theme_light?0x02f3:0x07ff)
#define RED (g->theme_light?0xa000:0xfbc8)
#define ORANGE (g->theme_light?0xb2c0:0xfd20)
#define VIOLET (g->theme_light?0x6a13:0xc31f)
#define SELECT_BG (g->theme_light?0xffe0:0x0010)
#define CARD_BG BG
#define FG_DIM (g->theme_light?0x12ef:0xa6bf)
#define GOLD_DIM (g->theme_light?0xc58f:0x7be0)
#define BLUE_DIM (g->theme_light?0x7d79:0x03ef)
#define GREEN_DIM (g->theme_light?0x7dd3:0x03e0)
#define ORANGE_DIM (g->theme_light?0xd54f:0x7a80)
#define VIOLET_DIM (g->theme_light?0xb4f9:0x618f)
static uint16_t category(direct_gfx_t*g,unsigned i){
 const uint16_t colors[]={BLUE,GOLD,GREEN,VIOLET,BLUE,ORANGE};return colors[i%6];
}
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
 direct_gfx_rect(g,0,0,320,26,BG);
 direct_gfx_rect(g,0,0,4,26,BLUE);
 if(visible(g,6,24)){
  char text[512];localized(g,s,text);fit(text,14,info&&*info?216:296);
  direct_gfx_text(g,12,6,text,14,FG);if(info)right(g,308,7,info,12,BLUE);
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
 direct_gfx_rect(g,0,220,320,20,BG);direct_gfx_rect(g,0,220,320,1,LINE);
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
  direct_gfx_text(g,left+(rightedge-left-width)/2,221,text,12,!strcmp(hints[i],"--")?DIM:i==5?BLUE:MUTED);
  if(i)direct_gfx_rect(g,left,223,1,15,LINE);
 }
}
static void number(char*out,size_t cap,int32_t value,unsigned scale,const char*unit,bool valid){
 if(!valid){snprintf(out,cap,"-- %s",unit);return;}
 uint32_t n=value<0?(uint32_t)(-(int64_t)value):(uint32_t)value;
 if(scale==100)snprintf(out,cap,"%s%lu.%02lu %s",value<0?"-":"",(unsigned long)(n/100),(unsigned long)(n%100),unit);
 else if(scale==10)snprintf(out,cap,"%s%lu.%lu %s",value<0?"-":"",(unsigned long)(n/10),(unsigned long)(n%10),unit);
 else snprintf(out,cap,"%ld %s",(long)value,unit);
}
static void metric(direct_gfx_t*g,int x,int y,int32_t n,unsigned scale,const char*unit,bool valid,int size,bool centered){
 char b[32];number(b,sizeof(b),n,scale,unit,valid);
 if(centered)center(g,x,y,b,size,valid?FG:MUTED);else t(g,x,y,b,size,valid?FG:MUTED);
}
/* Cards use clipped rectangles so the same geometry renders in 16-row strips. */
static void card(direct_gfx_t*g,int x,int y,int w,int h,uint16_t color){
 direct_gfx_rect(g,x+3,y,w-6,h,color);direct_gfx_rect(g,x,y+3,w,h-6,color);
 direct_gfx_rect(g,x+1,y+1,w-2,h-2,color);
}
/* Home positions are native LCD pixels and text coordinates are baselines. */
#define HOME_ASSET_Y 32 /* Header height; energy routes are relative to the asset. */
typedef struct {int16_t x,y;} home_point_t;
static void home_stroke(direct_gfx_t*g,int x,int y,int x1,int y1,int width,uint16_t color){
 int radius=width/2,top=y<y1?y:y1,bottom=y>y1?y:y1;
 if(!visible(g,top-radius,bottom-top+width))return;
 int dx=x1-x,dy=y1-y;
 if(!dy)direct_gfx_rect(g,(x<x1?x:x1)-radius,y-radius,(dx<0?-dx:dx)+width,width,color);
 else if(!dx)direct_gfx_rect(g,x-radius,top-radius,width,(dy<0?-dy:dy)+width,color);
 else for(int offset=-radius;offset<=radius;offset++){
  if((dx<0?-dx:dx)>=(dy<0?-dy:dy))direct_gfx_line(g,x,y+offset,x1,y1+offset,color);
  else direct_gfx_line(g,x+offset,y,x1+offset,y1,color);
 }
}
static void home_cable(direct_gfx_t*g,const home_point_t *p,unsigned n,uint16_t color,bool active,bool animate,bool reverse,uint8_t phase){
 unsigned lengths[5],total=0;
 for(unsigned i=0;i+1<n;i++){
  int dx=p[i+1].x-p[i].x,dy=p[i+1].y-p[i].y;
  unsigned ax=dx<0?-dx:dx,ay=dy<0?-dy:dy;
  lengths[i]=ax>ay?ax:ay;total+=lengths[i];
  /* Black edge separates the 3px cable from every part of the raster house. */
  home_stroke(g,p[i].x,p[i].y+HOME_ASSET_Y,p[i+1].x,p[i+1].y+HOME_ASSET_Y,7,0x0000);
  home_stroke(g,p[i].x,p[i].y+HOME_ASSET_Y,p[i+1].x,p[i+1].y+HOME_ASSET_Y,3,active?color:0x9cf3);
 }
 if(!active || !animate || !total)return;
 for(unsigned dot=0;dot<2;dot++){
  unsigned pos=((phase+dot*8U)%16U)*total/16U;
  if(reverse)pos=total-pos;
  for(unsigned i=0;i+1<n;i++){
   if(pos<=lengths[i] && lengths[i]){
    int x=p[i].x+(p[i+1].x-p[i].x)*(int)pos/(int)lengths[i];
    int y=p[i].y+HOME_ASSET_Y+(p[i+1].y-p[i].y)*(int)pos/(int)lengths[i];
    int dx=p[i+1].x-p[i].x,dy=p[i+1].y-p[i].y;
    /* Directional 9px chevrons remain recognizable on a TN panel. */
    int sx=dx>0?1:dx<0?-1:0,sy=dy>0?1:dy<0?-1:0;
    if(reverse){sx=-sx;sy=-sy;}
    int ax=x-sx*4+sy*4,ay=y-sy*4-sx*4;
    int bx=x-sx*4-sy*4,by=y-sy*4+sx*4;
    home_stroke(g,ax,ay,x,y,5,0x0000);home_stroke(g,bx,by,x,y,5,0x0000);
    home_stroke(g,ax,ay,x,y,3,0xffff);home_stroke(g,bx,by,x,y,3,0xffff);break;
   }
   pos-=lengths[i];
  }
 }
}
static void home_wifi(direct_gfx_t*g,const direct_ui_data_t*d,uint16_t fg,uint16_t muted){
 int level=direct_home_wifi_level(d);uint16_t dim=g->theme_light?0xc6db:0x320a;
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
 t(g,10,4,"Energy overview",14,fg);
 char clock[16]="--:--";if(d->clock_valid)snprintf(clock,sizeof(clock),"%02d:%02d",d->hour%24,d->minute%60);
 direct_home_text(g,282-direct_home_text_width(clock,12),18,clock,12,muted);
 home_wifi(g,d,fg,muted);
 direct_home_asset_draw(g,0,HOME_ASSET_Y,bg);
 static const home_point_t pv[]={{170,65},{140,68},{137,73},{137,88}};
 static const home_point_t grid[]={{56,83},{57,102},{123,105}};
 static const home_point_t battery[]={{134,116},{134,138},{130,141}};
 static const home_point_t load[]={{145,109},{164,112},{178,112},{178,137},{218,137},{225,144}};
 direct_home_values_t v;direct_home_values(d,&v);
 bool live=d->dsp_online||d->dsp_display_held,animate=live&&!d->ota_busy;
 home_cable(g,pv,4,0xffe0,live&&p->Ppv,animate,false,d->home_flow_phase);
 home_cable(g,grid,3,0x07ff,live&&p->ACInWatt,animate,p->ACInWatt<0,d->home_flow_phase);
 home_cable(g,battery,3,0x07e0,live&&v.battery_watts,animate,v.battery_state==DIRECT_HOME_DISCHARGING,d->home_flow_phase);
 home_cable(g,load,6,0xfd20,live&&p->OutputWatt,animate,p->OutputWatt<0,d->home_flow_phase);
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
}
static void menu(direct_gfx_t*g,const direct_ui_state_t*u){
 title(g,"System menu",NULL);
 const direct_icon_t icons[]={DIRECT_ICON_INVERTER,DIRECT_ICON_SOLAR,DIRECT_ICON_BATTERY,
  DIRECT_ICON_GRID,DIRECT_ICON_INVERTER,DIRECT_ICON_WARNING};
 unsigned count=direct_ui_menu_count();
 for(unsigned i=0;i<count&&i<6;i++){
  int x=10+(int)(i%2)*153,y=35+(int)(i/2)*59;bool sel=i==u->selected;
  uint16_t ink=sel?BG:FG,color=sel?ink:category(g,i);card(g,x,y,147,53,sel?FG:BG);
  direct_gfx_rect(g,x+4,y+10,3,33,sel?color:LINE);
  direct_gfx_icon(g,icons[i],x+13,y+8,color);
  char label[512];localized(g,direct_ui_menu_name(i),label);
  if(direct_gfx_text_width(label,16)<=94)direct_gfx_text(g,x+46,y+17,label,16,ink);
  else {
   char *split=strchr(label,' ');if(!split)split=strchr(label,'/');
   if(split){*split=0;direct_gfx_text(g,x+46,y+7,label,16,ink);
    while(split[1]==' '||split[1]=='/'){split++;}
    direct_gfx_text(g,x+46,y+29,split+1,16,ink);}
   else {fit(label,16,94);direct_gfx_text(g,x+46,y+17,label,16,ink);}
  }
 }
 footer(g,"UP/DN Select","ENTER Open",u->message);
}
static void submenu(direct_gfx_t*g,const direct_ui_state_t*u){
 unsigned count=direct_ui_submenu_count(u->menu_selected),start=u->selected/DIRECT_UI_SUBMENU_ROWS*DIRECT_UI_SUBMENU_ROWS;
 char info[20];snprintf(info,sizeof(info),"%u / %u",start/DIRECT_UI_SUBMENU_ROWS+1,(count+DIRECT_UI_SUBMENU_ROWS-1)/DIRECT_UI_SUBMENU_ROWS);
 title(g,direct_ui_menu_name(u->menu_selected),info);
 for(unsigned i=0;i<DIRECT_UI_SUBMENU_ROWS&&start+i<count;i++){
  int y=33+(int)i*31;bool selected=start+i==u->selected;
  uint16_t fg=selected?BG:FG,bg=selected?FG:BG;
  direct_gfx_rect(g,10,y,300,29,bg);
  t(g,24,y+4,direct_ui_submenu_name(u->menu_selected,start+i),16,fg);
  right(g,298,y+4,">",16,fg);
 }
 footer(g,"UP/DN Select","ENTER Open",u->message);
}
static const char*group_titles[]={"Input","Output","Charging","Battery","Generator","Other","Equalization","All parameters","Common settings","Parallel settings","Socket settings","Schedule registers","Battery voltages"};
static void config(direct_gfx_t*g,const direct_ui_state_t*u,const direct_ui_data_t*d){
 unsigned count=direct_ui_group_count(u->group),start=u->selected/DIRECT_UI_CONFIG_ROWS*DIRECT_UI_CONFIG_ROWS;char info[20];
 snprintf(info,sizeof(info),"%u / %u",u->selected/DIRECT_UI_CONFIG_ROWS+1,(count+DIRECT_UI_CONFIG_ROWS-1)/DIRECT_UI_CONFIG_ROWS);
 title(g,u->page==UI_REVIEW?"Review change":u->group<DIRECT_UI_GROUPS?group_titles[u->group]:"Settings",info);
 int selected=direct_ui_group_param(u->group,u->selected);
 if((u->editing||u->pending||u->page==UI_REVIEW)&&selected>=0){
  bool review=u->page==UI_REVIEW;
  char current[40],draft[40];
  direct_ui_config_value((unsigned)selected,u->draft,draft,sizeof(draft));
  if(d->valid[selected])direct_ui_config_value((unsigned)selected,d->config[selected],current,sizeof(current));
  else snprintf(current,sizeof(current),"--");
  center(g,160,37,direct_ui_param_name((unsigned)selected),16,FG);
  card(g,12,64,296,43,CARD_BG);
  char current_label[512];localized(g,"Current value",current_label);
  if(direct_gfx_text_width(current_label,16)+direct_gfx_text_width(current,16)>260){
   t(g,24,66,"Current value",16,MUTED);right(g,296,86,current,16,FG);
  }else {t(g,24,77,"Current value",16,MUTED);right(g,296,77,current,16,FG);}
  card(g,12,114,296,66,SELECT_BG);
  t(g,24,120,"New value",16,GREEN);
  int size=direct_gfx_text_width(draft,28)<=260?28:16;
  center(g,160,144,draft,size,FG);
  center(g,160,189,u->pending?"Sending settings":review?"Review change":"L/R step  UP/DN fast",16,u->pending?BLUE:GOLD);
  footer(g,u->pending?"Sending to inverter":review?"ENTER Confirm":"ENTER Review",u->pending?"":"ESC Cancel",u->message);return;
 }
 for(unsigned i=0;i<DIRECT_UI_CONFIG_ROWS&&start+i<count;i++){
  int idx=direct_ui_group_param(u->group,start+i);if(idx<0)continue;
  int y=33+(int)i*31;bool sel=start+i==u->selected;
  if(!visible(g,y,29))continue;
  uint16_t fg=sel?BG:FG,bg=sel?FG:BG;
  direct_gfx_rect(g,10,y,300,29,bg);
  char label[512],val[32],value[512];
  if(d->valid[idx])direct_ui_config_value((unsigned)idx,d->config[idx],val,sizeof(val));else snprintf(val,sizeof(val),"--");
  localized(g,val,value);fit(value,16,150);
  int vw=direct_gfx_text_width(value,16);
  localized(g,direct_ui_param_name((unsigned)idx),label);fit(label,16,264-vw);
  direct_gfx_text(g,24,y+4,label,16,fg);
  direct_gfx_text(g,296-vw,y+4,value,16,fg);
  /* Small lock mark preserves the full name/value row; the editor explains why. */
  if(!d->writable[idx])direct_gfx_rect(g,14,y+12,4,5,fg);
 }
 footer(g,"UP/DN Select","ENTER Edit",u->message);
}
/* Six full-width inverted rows keep selection readable on the TN panel. A
 * review row separates its label from the saved/draft values; every field is
 * reachable with the same page keys used by the surrounding settings pages. */
static void battery(direct_gfx_t*g,const direct_ui_state_t*u,const direct_ui_view_t*v){
 bool review=u->page==UI_BATTERY_REVIEW;
 title(g,review?"Review battery":"Battery configuration",strrchr(v->title,' '));
 for(unsigned r=0;r<DIRECT_UI_CONFIG_ROWS;r++){
  if(!v->rows[r][0])continue;
  int y=32+(int)r*27;bool sel=v->selected==(int)r;
  uint16_t fg=sel?BG:FG;
  direct_gfx_rect(g,10,y,300,26,sel?FG:BG);
  char label[96];snprintf(label,sizeof(label),"%s",v->rows[r]);char *value=strchr(label,':');
  if(value){*value++=0;while(*value==' ')value++;}
  char text[512];localized(g,label,text);
  if(review && value){
   fit(text,12,134);direct_gfx_text(g,16,y+6,text,12,fg);
   char values[512];localized(g,value,values);fit(values,12,150);
   direct_gfx_text(g,302-direct_gfx_text_width(values,12),y+6,values,12,fg);
  }else{
   char values[512];localized(g,value?value:"",values);fit(values,16,138);
   int width=direct_gfx_text_width(values,16);fit(text,16,278-width);
   direct_gfx_text(g,18,y+3,text,16,fg);
   if(value)direct_gfx_text(g,302-width,y+3,values,16,fg);
  }
 }
 t(g,14,196,v->rows[7],12,MUTED);
 footer(g,"","",u->message);
}
static void list(direct_gfx_t*g,const direct_ui_state_t*u,const direct_ui_data_t*d,const direct_ui_view_t*v){
 const char*name=u->page==UI_PARAMETERS?"Running parameters":u->page==UI_HELP?"Device information":u->page==UI_TIME?"Time settings":u->page==UI_ALARM?"Active alarms":u->page==UI_PRODUCTION?"Manufacturing":"Information";
 char info[18]="";if(u->page==UI_PARAMETERS)snprintf(info,sizeof(info),"%u / 5",u->detail_page+1);
 if(u->page==UI_HOME)name=d->network_online?"Network online":"Network offline";
 title(g,name,info);
 if(u->page==UI_PARAMETERS && u->detail_page==4){
  direct_gfx_icon(g,DIRECT_ICON_GENERATOR,23,46,(d->status_valid || d->dsp_status_held)?VIOLET:VIOLET_DIM);
  t(g,74,48,"Generator",16,FG);
  t(g,74,78,!(d->status_valid || d->dsp_status_held)?"Input status unknown":(d->status.inverter_status[0]&2)?"Generator input valid":"Generator input not valid",12,MUTED);
  rule(g,102);
  t(g,16,118,"Utility input",14,MUTED);
  right(g,304,118,!(d->status_valid || d->dsp_status_held)?"--":(d->status.inverter_status[0]&1)?"Valid":"Not valid",14,FG);
  t(g,16,149,"Generator power",14,MUTED);right(g,304,149,"--",14,FG);
  t(g,16,179,"Generator energy",14,MUTED);right(g,304,179,"--",14,FG);
  t(g,16,204,"No independent metering",12,MUTED);
  footer(g,u->parent_page==UI_SUBMENU?"ESC Back":"UP/DN Group",u->parent_page==UI_SUBMENU?NULL:"L/R Page",NULL);return;
 }
 if(u->page==UI_ALARM && !d->status_valid && !d->bms_online){
  direct_gfx_icon(g,DIRECT_ICON_WARNING,144,63,GOLD);
  center(g,160,112,"Alarm status unavailable",14,FG);
  center(g,160,145,"Waiting for live status",12,MUTED);
  footer(g,"Communication unavailable","ESC Back",NULL);return;
 }
 for(unsigned r=0;r<9;r++){
  int y=35+(int)r*20;bool sel=v->selected==(int)r;
  if(!visible(g,y-1,21))continue;
  if(sel)direct_gfx_rect(g,10,y-1,300,19,SELECT_BG);
  const char*s=v->rows[r];if(!*s)continue;
  /* Preserve all original fields while hiding stale live measurements. */
  bool stale=(u->page==UI_PARAMETERS&&!(d->dsp_online || d->dsp_display_held)&&
              (u->detail_page<2 || (u->detail_page==3 && r>=4 && r<=6))) ||
             (u->page==UI_HOME&&!(d->dsp_online || d->dsp_display_held));
  const char*colon=strchr(s,':');
  if(colon){
   char label[48];size_t len=(size_t)(colon-s);if(len>=sizeof(label))len=sizeof(label)-1;
   memcpy(label,s,len);label[len]=0;
   const char*value=stale?"--":colon+1;while(*value==' ')value++;
   char ll[512],vv[512];localized(g,label,ll);localized(g,value,vv);
   int width=direct_gfx_text_width(vv,12);
   if(width<140 && direct_gfx_text_width(ll,12)+width<275){
    t(g,16,y,label,12,MUTED);right(g,304,y,value,12,sel?GREEN:FG);
   }
   else t(g,16,y,stale?"--":s,12,stale?MUTED:FG);
  }else t(g,16,y,stale?"Live data unavailable":s,12,u->page==UI_ALARM?GOLD:FG);
 }
 footer(g,u->page==UI_PRODUCTION?"":u->page==UI_ALARM?"UP/DN Scroll":u->page==UI_TIME?"L/R Edit":u->page==UI_HELP?"L/R LCD Test":"UP/DN Group",u->page==UI_PARAMETERS||u->page==UI_ALARM?"L/R Page":"ESC Back",u->message);
}
static void bms(direct_gfx_t*g,const direct_ui_state_t*u,const direct_ui_data_t*d,const direct_ui_view_t*v){
 title(g,"Battery monitoring",d->bms_online?"ONLINE":"OFFLINE");
 if(!d->bms_online){
  metric(g,16,39,0,0,"%",false,28,false);metric(g,117,43,0,0,"V",false,22,false);
  direct_gfx_icon(g,DIRECT_ICON_WARNING,144,90,GOLD);
  center(g,160,139,"BMS not responding",14,FG);
  center(g,160,165,"Live data unavailable",12,MUTED);
  footer(g,"Waiting for BMS","ESC Back",NULL);return;
 }
 if(u->detail_page==0){
  /* Keep summary/protection/current/SOH information reachable. */
  for(unsigned r=0;r<8;r++){
   int y=38+(int)r*21;const char*s=v->rows[r],*colon=strchr(s,':');
   if(!visible(g,y,20))continue;
   if(!colon){t(g,16,y,s,12,FG);continue;}
   char label[48];size_t n=(size_t)(colon-s);if(n>47)n=47;memcpy(label,s,n);label[n]=0;
   char ll[512],vv[512];localized(g,label,ll);localized(g,colon+1,vv);
   fit(ll,12,276-direct_gfx_text_width(vv,14));
   direct_gfx_text(g,16,y,ll,12,MUTED);right(g,304,y,colon+1,14,FG);
  }
  footer(g,"ENTER Cells","L/R Page",NULL);return;
 }
 uint16_t min=65535,max=0;int minidx=-1,maxidx=-1;unsigned known=0;
 for(unsigned i=0;i<16;i++)if(d->bms.cell_mv[i]){
  uint16_t mv=d->bms.cell_mv[i];known++;
  if(mv<min){min=mv;minidx=(int)i;}if(mv>max){max=mv;maxidx=(int)i;}
 }
 uint16_t soc=direct_bms_percent(&d->bms,d->bms.soc_percent);
 metric(g,12,37,soc,0,"%",soc<=100,28,false);
 metric(g,117,42,d->bms.voltage_10mv,100,"V",true,14,false);
 metric(g,231,42,(int32_t)max-min,0,"mV",known>0,14,false);
 for(unsigned i=0;i<16;i++){
  int x=12+(int)(i%4)*75,y=84+(int)(i/4)*30;
  char id[5],val[12];snprintf(id,sizeof(id),"%02u",i+1);t(g,x,y,id,12,MUTED);
  uint16_t mv=d->bms.cell_mv[i];
  if(mv)snprintf(val,sizeof(val),"%u.%03u",mv/1000,mv%1000);else snprintf(val,sizeof(val),"--");
  uint16_t color=(int)i==maxidx?GOLD:(int)i==minidx?BLUE:FG;
  right(g,x+68,y,val,12,color);direct_gfx_rect(g,x,y+22,68,1,LINE);
 }
 char extrema[40];if(known)snprintf(extrema,sizeof(extrema),"MIN %02d  MAX %02d",minidx+1,maxidx+1);else snprintf(extrema,sizeof(extrema),"No cell measurements");
 footer(g,extrema,"UP/DN Group",NULL);
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
 t(g,12,181,"PV",14,GOLD);t(g,76,181,"GRID",14,BLUE);
 t(g,159,181,"BAT",14,GREEN);t(g,239,181,"LOAD",14,ORANGE);
 t(g,12,202,"Compare same angle in A and B",12,FG);
 footer(g,"L/R Info, A, B","ESC Exit",NULL);
}
static void factory(direct_gfx_t*g,const direct_ui_state_t*u){
 title(g,u->group==2?"LCD reset":u->group?"Restart controller":"Reset UI preferences",NULL);
 direct_gfx_icon(g,DIRECT_ICON_WARNING,144,46,GOLD);
 center(g,160,94,u->group==2?"Reset display?":u->group?"Restart communication?":"Reset local preferences?",14,FG);
 center(g,160,120,"DSP power settings retained",12,MUTED);
 direct_gfx_rect(g,65,154,84,36,u->confirm?BG:SELECT_BG);
 direct_gfx_rect(g,172,154,84,36,u->confirm?SELECT_BG:BG);
 center(g,107,163,"NO",14,u->confirm?MUTED:GREEN);
 center(g,214,163,"YES",14,u->confirm?GREEN:MUTED);
 footer(g,"L/R Choose","ENTER Confirm",u->message);
}
static void appearance(direct_gfx_t*g,const direct_ui_state_t*u,const direct_ui_data_t*d){
 title(g,"Appearance",NULL);
 const char*names[]={"Theme","Language","Save"};
 const char*values[]={d->theme_light?"Light":"Dark",d->language_zh?"Chinese":"English","ENTER Confirm"};
 for(unsigned i=0;i<3;i++){
  int y=48+(int)i*46;bool selected=u->selected==i;
  if(selected){direct_gfx_rect(g,10,y-8,300,36,SELECT_BG);direct_gfx_rect(g,10,y-3,3,25,GREEN);}
  t(g,24,y,names[i],14,FG);right(g,296,y,values[i],14,selected?GREEN:MUTED);
 }
 footer(g,"UP/DN Select","ESC Cancel",u->message);
}
static void upgrade(direct_gfx_t*g,const direct_ui_data_t*d,const direct_ui_view_t*v){
 title(g,"Firmware upgrade",NULL);
 center(g,160,38,v->rows[0],16,d->ota.state==OTA_STATE_FAILED?ORANGE:GREEN);
 /* Reserve independent widths for source/target and version. A long version
  * must not overwrite the connection label. */
 if(visible(g,65,28)){
  char source[512],version[512];localized(g,v->rows[1],source);localized(g,v->rows[3],version);
  fit(source,14,180);fit(version,14,100);
  direct_gfx_text(g,16,65,source,14,BLUE);
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
void direct_ui_scene_draw(direct_gfx_t*g,const direct_ui_state_t*u,const direct_ui_data_t*d,const direct_ui_view_t*v){
 g->theme_light=d->theme_light;g->language_zh=d->language_zh;
 direct_gfx_rect(g,0,0,320,240,BG);
 switch(u->page){
 case UI_HOME:if(!u->main_mode)home(g,d);else list(g,u,d,v);break;
 case UI_MENU:menu(g,u);break;
 case UI_SUBMENU:submenu(g,u);break;
 case UI_CONFIG:case UI_REVIEW:config(g,u,d);break;
 case UI_BATTERY:case UI_BATTERY_REVIEW:battery(g,u,v);break;
 case UI_BMS:bms(g,u,d,v);break;
 case UI_FACTORY:factory(g,u);break;
 case UI_UPGRADE:upgrade(g,d,v);break;
 case UI_APPEARANCE:appearance(g,u,d);break;
 case UI_HELP:if(u->detail_page)lcd_test(g,u->detail_page==2);else list(g,u,d,v);break;
 default:list(g,u,d,v);break;
 }
 physical_footer(g,u,d);
}


