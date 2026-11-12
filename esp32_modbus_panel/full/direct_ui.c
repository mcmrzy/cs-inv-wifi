/* CS-MB-PANEL owns only its local UI. Inverter pages are panel_ui/catalog. */
#include "full_ui_bridge.h"
#include "full_write_result.h"
#include "direct_ui.h"
#include "panel_display_settings.h"
#include "panel_catalog.h"
#include "direct_key_codec.h"
#include <stdio.h>
#include <string.h>

static panel_display_settings_t appearance_display(const direct_ui_state_t *u){
 panel_display_settings_t value={(uint8_t)u->time_fields[0],(uint8_t)u->time_fields[1]};
 return panel_display_valid(value)?value:panel_display_preferences();
}
static const char *const menus[] = {"Running parameters", "Inverter settings", "Power control", "Faults / warnings", "Auxiliary settings", "Service"};
static const char *const sub4[]={"Appearance","Time set","Device information","Phone connection","Firmware upgrade","Wi-Fi setup","Panel BMS protocol"};
static const char *const sub5[]={"Advanced inverter settings","Communication settings","Communication diagnostics","Screen test","Restart controller","Reset local UI settings","LCD reset","Manufacturing"};
static const char *const *const submenus[]={NULL,NULL,NULL,NULL,sub4,sub5};
static const uint8_t subcounts[]={0,0,0,0,sizeof(sub4)/sizeof(*sub4),sizeof(sub5)/sizeof(*sub5)};
unsigned direct_ui_submenu_count(unsigned menu){return menu<6?subcounts[menu]:0;}
const char *direct_ui_submenu_name(unsigned menu,unsigned position){return position<direct_ui_submenu_count(menu)?submenus[menu][position]:NULL;}
unsigned direct_ui_menu_count(void){return 5;}
const char *direct_ui_menu_name(unsigned i){return i<6?menus[i]:NULL;}
/* Kept as ABI stubs for the shared display signature. No legacy parameters exist. */
const char *direct_ui_param_name(unsigned i){(void)i;return "Unsupported";}
unsigned direct_ui_group_count(unsigned group){(void)group;return 0;}
int direct_ui_group_param(unsigned group,unsigned position){(void)group;(void)position;return -1;}
bool direct_ui_value_allowed(unsigned index,uint16_t value){(void)index;(void)value;return false;}
void direct_ui_config_value(unsigned index,uint16_t value,char *out,size_t size){
 (void)index;(void)value;if(out && size)snprintf(out,size,"--");
}
static void note(direct_ui_state_t *u,const char *message){snprintf(u->message,sizeof(u->message),"%s",message);u->dirty=true;}
void direct_ui_init(direct_ui_state_t *u){if(u){memset(u,0,sizeof(*u));u->dirty=true;}}
bool direct_ui_has_alarm(const direct_ui_data_t *d){
 return d && ((d->status_valid && (d->status.fault || d->status.warning[0] || d->status.warning[1] || d->status.warning[2])) ||
  (d->dsp_online && (d->run.FaultValue || d->run.Warning || d->run.BmsWarning)) ||
  (d->bms_online && (d->bms.warning || d->bms.protection)));
}
void direct_ui_write_result(direct_ui_state_t *u,int result){
 if(!u || !u->pending || result==1)return;
 u->pending=false;u->editing=false;
 note(u,result==FULL_WRITE_PROTOCOL_ACCEPTED?"Accepted by inverter":
  result==FULL_WRITE_UNCONFIRMED || result==2 || result==UI_WRITE_SUBMIT_FAILED?"Result unconfirmed":
  result==0?"Local settings saved":result==UI_WRITE_SAVE_FAILED?"Save failed / retry":"Save rejected");
}
static unsigned wrap(unsigned n,int delta,unsigned count){return count?(unsigned)((int)n+delta+(int)count)%count:0;}
static void return_settings(direct_ui_state_t *u){
 u->page=u->parent_page==UI_SUBMENU?UI_SUBMENU:UI_MENU;
 u->selected=u->page==UI_SUBMENU?u->submenu_selected:u->menu_selected;
 u->editing=false;u->confirm=false;
}
static uint8_t page_step(unsigned selected,int delta,unsigned count,unsigned rows){
 unsigned page=wrap(selected/rows,delta,(count+rows-1)/rows),next=page*rows+selected%rows;
 return (uint8_t)(next<count?next:count-1);
}
static bool ota_cancellable(const direct_ui_data_t *d){
 return d->ota_busy && (d->ota.state==OTA_STATE_ACCEPTED || d->ota.state==OTA_STATE_DOWNLOADING || d->ota.state==OTA_STATE_RECEIVING);
}
static bool local_page(direct_ui_page_t p){
 return p==UI_HOME || p==UI_MENU || p==UI_SUBMENU || p==UI_APPEARANCE || p==UI_TIME || p==UI_HELP ||
  p==UI_FACTORY || p==UI_UPGRADE || p==UI_PRODUCTION || p==UI_PHONE_SETUP || p==UI_BMS || p==UI_WEATHER || p==UI_BMS_PROTOCOL;
}
static unsigned month_days(int year,int month){
 static const unsigned days[]={31,28,31,30,31,30,31,31,30,31,30,31};
 if(month<1 || month>12)return 31;
 return days[month-1]+(month==2 && year%4==0 && (year%100!=0 || year%400==0));
}
void direct_ui_key_hints(const direct_ui_state_t *u,const direct_ui_data_t *d,const char *h[6]){
 for(unsigned i=0;i<6;i++)h[i]="--";
 if(u->pending)return;
 h[0]="Back";
 switch(u->page){
 case UI_HOME:h[0]="Weather";h[1]="Alarm";h[2]="Modes";h[3]="Grid";h[4]=d->language_zh?"Battery":"Batt";h[5]="Menu";break;
 case UI_WEATHER:h[5]="Refresh";break;
 case UI_BMS:
  if(full_battery_pages(d)>1){h[1]=h[2]="Prev";h[3]=h[4]="Next";}
  h[5]="Link";break;
 case UI_BMS_PROTOCOL:
  if(d->bms_detection!=PANEL_BMS_DETECT_RUNNING){
   h[2]="Up";h[3]="Down";h[5]=u->editing?"Save":"Open";
  }
  break;
 case UI_MENU:h[5]="Open";
  if(u->selected!=1)h[1]="Left";
  if(u->selected!=2 && u->selected!=4)h[4]="Right";
  if(u->selected==3 || u->selected==4)h[2]="Up";
  if(u->selected==0 || u->selected==2)h[3]="Down";
  break;
 case UI_SUBMENU:
  if(direct_ui_submenu_count(u->menu_selected)>DIRECT_UI_SUBMENU_ROWS){h[1]="Prev";h[4]="Next";}
  h[2]="Up";h[3]="Down";h[5]="Open";break;
 case UI_FACTORY:h[1]="NO";h[4]="YES";h[5]=u->confirm?(d->language_zh?"Confirm":"OK"):(d->language_zh?"Choose":"Select");break;
 case UI_TIME:h[1]="-";h[2]="Up";h[3]="Down";h[4]="+";h[5]="Save";break;
 case UI_APPEARANCE:
  h[0]="Cancel";h[2]="Up";h[3]="Down";h[5]="Save";
  if(u->selected==2 || u->selected==4){h[1]="-";h[4]="+";}
  else if(u->selected<PANEL_APPEARANCE_ROWS-1)h[1]=h[4]="Change";
  break;
 case UI_HELP:if(u->menu_selected==5){h[1]="Prev";h[4]="Next";}break;
 case UI_UPGRADE:
  if(d->ota_busy)h[0]=u->confirm && ota_cancellable(d)?"Back":"--";
  if(ota_cancellable(d))h[5]=u->confirm?"Confirm":"Cancel";
  break;
 default:break;
 }
 if(d->ota_busy && (u->page==UI_FACTORY || u->page==UI_TIME || u->page==UI_APPEARANCE))for(unsigned i=1;i<6;i++)h[i]="--";
}
direct_ui_event_t direct_ui_key(direct_ui_state_t *u,uint8_t key,uint32_t now,const direct_ui_data_t *d){
 direct_ui_event_t e={0};(void)now;
 if(!u || !d || !key)return e;
 if(key==DIRECT_KEY_ENTER_BACK){
  if(u->page==UI_HELP && u->menu_selected==4 && !d->ota_busy){
   u->menu_selected=5;u->page=UI_SUBMENU;u->selected=0;u->submenu_selected=0;u->dirty=true;
  }
  return e;
 }
 if(key>DIRECT_KEY_RIGHT)return e;
 if(!local_page(u->page)){u->page=UI_HOME;u->selected=0;u->editing=false;u->pending=false;u->dirty=true;return e;}
 if(u->page==UI_MENU)u->selected%=direct_ui_menu_count();
 if(u->page==UI_TIME)u->selected%=7;
 if(u->page==UI_APPEARANCE)u->selected%=PANEL_APPEARANCE_ROWS;
 if(u->pending){note(u,"Saving local settings");return e;}
 u->dirty=true;u->message[0]=0;
 if(d->ota_busy && (u->page==UI_FACTORY || u->page==UI_TIME || u->page==UI_APPEARANCE) && key!=1){note(u,"Upgrade busy");return e;}
 switch(u->page){
 case UI_HOME:
  if(key==1){u->page=UI_WEATHER;u->detail_page=0;}
  else if(key==2){u->page=UI_MENU;u->selected=u->menu_selected;u->menu_origin=UI_HOME;}
  else if(key==6){u->page=UI_BMS;u->parent_page=UI_HOME;u->detail_page=0;}
  else if(key>=3){
   e.action=FULL_UI_OPEN_MODBUS;e.index=key==4?PANEL_PAGE_ALARMS:PANEL_PAGE_DETAILS;
   e.value=key==6?3:key==3?PANEL_DETAIL_PAGES-1:0;
  }
  break;
 case UI_WEATHER:
  if(key==1){u->page=UI_HOME;u->detail_page=0;}
  break;
 case UI_BMS:
  if(key==1){
   u->detail_page=0;
   if(u->parent_page==UI_PARAMETERS){
    u->page=UI_MENU;u->selected=0;u->parent_page=UI_HOME;e.action=FULL_UI_OPEN_MODBUS;e.index=PANEL_PAGE_DETAIL_GROUPS;e.value=3;
   }else{u->page=UI_HOME;u->parent_page=UI_HOME;}
  }
  else if(key==DIRECT_KEY_ENTER){u->page=UI_BMS_PROTOCOL;u->selected=0;u->editing=false;}
  else{
   unsigned pages=full_battery_pages(d),page=full_battery_page(u,d);
   u->detail_page=(uint8_t)wrap(page,(key==3 || key==4)?-1:1,pages);
  }
  break;
 case UI_BMS_PROTOCOL:
  if(key==DIRECT_KEY_BACK){
   if(u->editing){u->editing=false;u->selected=0;}
   else if(u->parent_page==UI_SUBMENU)return_settings(u);
   else{u->page=UI_BMS;u->detail_page=0;}
  }
  else if(d->bms_detection==PANEL_BMS_DETECT_RUNNING)break;
 else if(key==DIRECT_KEY_UP || key==DIRECT_KEY_DOWN)u->selected=(uint8_t)wrap(u->selected,key==DIRECT_KEY_UP?-1:1,u->editing?2:3);
  else if(key==DIRECT_KEY_ENTER){
   if(!u->editing){
    if(u->selected==0)e.action=FULL_UI_DETECT_BMS;
    else if(u->selected==1){u->editing=true;u->selected=d->bms_protocol==PANEL_BMS_CSERGY;}
    else{e.action=FULL_UI_OPEN_MODBUS;e.index=PANEL_PAGE_SETTINGS;e.value=2;}
   }
   else{e.action=FULL_UI_SAVE_BMS_PROTOCOL;e.value=u->selected;}
  }
  break;
 case UI_MENU:
  if(key==1){u->page=UI_HOME;u->main_mode=0;}
  else if(key==2){
   u->menu_selected=u->selected;u->submenu_selected=0;
   if(u->selected<4){static const panel_page_t pages[]={PANEL_PAGE_DETAIL_GROUPS,PANEL_PAGE_SETTING_GROUPS,PANEL_PAGE_POWER,PANEL_PAGE_ALARMS};
    e.action=FULL_UI_OPEN_MODBUS;e.index=pages[u->selected];}
   else{u->page=UI_SUBMENU;u->selected=0;}
  }else if(key>=3 && key<=6){
   /* The tall settings tile is left of the four smaller tiles. */
   static const uint8_t neighbours[4][5]={
    {0,1,2,0,2}, /* Up */
    {1,1,0,1,3}, /* Left */
    {3,1,4,3,4}, /* Down */
    {2,0,2,4,4}, /* Right */
   };
   u->selected=neighbours[key-3][u->selected];
   u->menu_selected=u->selected;
  }
  break;
 case UI_SUBMENU:{
  unsigned count=direct_ui_submenu_count(u->menu_selected);
  if(!count){u->page=UI_MENU;u->selected=u->menu_selected;break;}
  u->selected=(uint8_t)(u->selected%count);
  if(key==1){
   if(u->menu_selected==5){
    u->page=UI_HELP;u->menu_selected=4;u->submenu_selected=2;u->detail_page=0;u->parent_page=UI_SUBMENU;
   }else{u->page=UI_MENU;u->selected=u->menu_selected;u->parent_page=UI_HOME;}
  }
  else if(key==2){
   unsigned m=u->menu_selected,n=u->selected;u->submenu_selected=(uint8_t)n;u->parent_page=UI_SUBMENU;
   if(m==5){
    if(n<3){
     static const panel_page_t service[]={PANEL_PAGE_SETTING_GROUPS,PANEL_PAGE_COMM,PANEL_PAGE_DIAG};
     e.action=FULL_UI_OPEN_MODBUS;e.index=service[n];break;
    }
    if(n==3){u->page=UI_HELP;u->detail_page=1;}
    else if(n==7){u->page=UI_PRODUCTION;u->detail_page=0;}
    else{u->page=UI_FACTORY;u->group=n==4?1:n==6?2:0;u->confirm=false;}
   }else{
    u->selected=0;
    if(n==0){u->page=UI_APPEARANCE;u->appearance_theme=d->theme_light;u->appearance_language=d->language_zh;
     panel_display_settings_t display=panel_display_preferences();u->time_fields[0]=display.brightness;
     u->time_fields[1]=d->valid[15] && !d->config[15]?0:display.timeout_index;
     panel_feedback_settings_t feedback=panel_feedback_preferences();
     u->time_fields[2]=feedback.led_brightness;u->time_fields[3]=feedback.beep_mode;
     u->time_fields[4]=0;}
    else if(n==1){
     u->page=UI_TIME;
     u->editing=!d->clock_valid;
     int fields[]={d->year,d->month,d->day,d->hour,d->minute,d->second,d->timezone_minutes};
     memcpy(u->time_fields,fields,sizeof(fields));
    }
    else if(n==2){u->page=UI_HELP;u->detail_page=0;}
    else if(n==3){u->page=UI_PHONE_SETUP;u->detail_page=0;}
    else if(n==4){u->page=UI_UPGRADE;u->detail_page=0;}
    else if(n==5){u->page=UI_WIFI;u->detail_page=0;}
    else if(n==6){u->page=UI_BMS_PROTOCOL;u->editing=false;}
   }
  }else{
   u->selected=key==4 || key==6?page_step(u->selected,key==4?-1:1,count,DIRECT_UI_SUBMENU_ROWS):(uint8_t)wrap(u->selected,key==3?-1:1,count);
   u->submenu_selected=u->selected;
  }
  break;
 }
 case UI_APPEARANCE:{
  panel_display_settings_t display=appearance_display(u);
  if(key==1){return_settings(u);e.action=UI_ACT_APPEARANCE_CANCEL;}
  else if(key==3 || key==5)u->selected=(uint8_t)wrap(u->selected,key==3?-1:1,PANEL_APPEARANCE_ROWS);
  else if((key==4 || key==6) && u->selected<PANEL_APPEARANCE_ROWS-1){
   if(u->selected==0)u->appearance_theme=!u->appearance_theme;
   else if(u->selected==1)u->appearance_language=!u->appearance_language;
   else if(u->selected==2){int b=display.brightness+(key==4?-10:10);uint8_t next=(uint8_t)(b<10?10:b>100?100:b);
    if(next==display.brightness)note(u,"Value limit reached");
    display.brightness=next;}
   else if(u->selected==3)display.timeout_index=(uint8_t)wrap(display.timeout_index,key==4?-1:1,PANEL_DISPLAY_TIMEOUT_COUNT);
   else if(u->selected==4){
    int percent=(u->time_fields[2]*100+127)/255;
    int value=percent+(key==4?-5:5);
    if(value<0)value=0;
    if(value>100)value=100;
    u->time_fields[2]=(value*255+50)/100;
    u->time_fields[4]|=PANEL_FEEDBACK_LED_EDIT;
   }else{
    u->time_fields[3]=(int)wrap((unsigned)u->time_fields[3],key==4?-1:1,PANEL_BEEP_COUNT);
    u->time_fields[4]|=PANEL_FEEDBACK_BEEP_EDIT;
   }
   e.action=UI_ACT_APPEARANCE_PREVIEW;
  }else if(key==2)e.action=UI_ACT_APPEARANCE_SAVE;
  u->time_fields[0]=display.brightness;u->time_fields[1]=display.timeout_index;
  e.index=panel_display_pack(display);e.value=(uint16_t)(u->appearance_theme | (u->appearance_language<<1));break;
 }
 case UI_FACTORY:
  if(key==1)return_settings(u);
  else if(key==4 || key==6)u->confirm=key==6;
  else if(key==2){if(u->confirm)e.action=u->group==2?UI_ACT_LCD_RESET:u->group==1?UI_ACT_RESTART:UI_ACT_LOCAL_RESET;else note(u,"Select YES to continue");}
  break;
 case UI_TIME:{
  static const int low[]={2020,1,1,0,0,0,-720},high[]={2099,12,31,23,59,59,840};
  if(key==1)return_settings(u);
  else if(key==3 || key==5)u->selected=(uint8_t)wrap(u->selected,key==3?-1:1,7);
  else if(key==4 || key==6){int i=u->selected,value=u->time_fields[i]+(key==4?-1:1)*(i==6?15:1);
   int maximum=i==2?(int)month_days(u->time_fields[0],u->time_fields[1]):high[i];
   if(value>=low[i] && value<=maximum){
    u->time_fields[i]=value;
    if(i<6)u->editing=true; /* A zone-only edit must not reset the UTC clock. */
    if(i<2 && (unsigned)u->time_fields[2]>month_days(u->time_fields[0],u->time_fields[1]))
     u->time_fields[2]=(int)month_days(u->time_fields[0],u->time_fields[1]);
   }else note(u,"Value limit reached");}
  else if(key==2){
   int64_t seconds;
   if(!u->editing || direct_ui_time_utc(u->time_fields,&seconds)){
    e.action=UI_ACT_SET_TIME;
    e.value=u->editing;
   }else note(u,"Invalid calendar date");
  }
  break;
 }
 case UI_UPGRADE:
  if(key==1){
   if(u->confirm && ota_cancellable(d))u->confirm=false;
   else if(d->ota_busy)note(u,"Keep device powered on");
   else return_settings(u);
  }else if(key==2){
   if(ota_cancellable(d)){
    if(u->confirm){u->confirm=false;e.action=UI_ACT_OTA_CANCEL;}
    else u->confirm=true;
   }else note(u,d->ota_busy?"Upgrade cannot be cancelled":"Send firmware from App or cloud");
  }
  break;
 case UI_HELP:
  if(key==1)return_settings(u);
  else if(u->menu_selected==5 && (key==4 || key==6))u->detail_page=u->detail_page==2?1:2;
  break;
 default:if(key==1)return_settings(u);break;
 }
 return e;
}
bool direct_ui_time_utc(const int f[7], int64_t *seconds) {
 if(!f || !seconds || f[0]<2020 || f[0]>2099 || f[1]<1 || f[1]>12 ||
    f[3]<0 || f[3]>23 || f[4]<0 || f[4]>59 || f[5]<0 || f[5]>59 || f[6]<-720 || f[6]>840)return false;
 static const unsigned days_in_month[]={31,28,31,30,31,30,31,31,30,31,30,31};
 bool leap=f[0]%4==0 && (f[0]%100!=0 || f[0]%400==0);
 unsigned max=days_in_month[f[1]-1]+(f[1]==2 && leap);
 if(f[2]<1 || (unsigned)f[2]>max)return false;
 int64_t days=0;for(int year=1970;year<f[0];year++)days+=365+(year%4==0 && (year%100!=0 || year%400==0));
 for(int month=1;month<f[1];month++)days+=days_in_month[month-1]+(month==2 && leap);
 days+=f[2]-1;*seconds=days*86400+f[3]*3600+f[4]*60+f[5]-f[6]*60;return true;
}
static const char *upgrade_error(const char *code){
 static const struct {const char *code,*label;} errors[]={
  {"DUPLICATE","Upgrade already requested"},{"INVALID_PARAMS","Invalid upgrade request"},
  {"INVALID_TARGET","Firmware target mismatch"},{"TLS_REQUIRED","Secure download required"},
  {"SIGNATURE_REQUIRED","Firmware signature missing"},{"SIGNATURE_INVALID","Firmware signature invalid"},
  {"SECURITY_VERSION_READ_FAILED","Security check unavailable"},{"ROLLBACK_REJECTED","Older firmware blocked"},
  {"OTA_BUSY","Another upgrade is running"},{"FILE_TOO_LARGE","Firmware file too large"},
  {"SIZE_MISMATCH","Firmware size mismatch"},{"HASH_MISMATCH","Firmware checksum mismatch"},
  {"HTTP_ERROR","Firmware download failed"},{"TIMEOUT","Upgrade timed out"},
  {"READ_FAILED","Firmware download interrupted"},{"FLASH_WRITE_FAILED","Firmware write failed"},
  {"TARGET_TRANSFER_FAILED","Target transfer failed"},{"PERSIST_FAILED","Upgrade settings save failed"},
  {"OTA_NOT_CANCELLABLE","Upgrade cannot be cancelled"},{"TASK_CREATE_FAILED","Upgrade could not start"},
  {"INTERNAL_ERROR","Upgrade internal error"},{"TARGET_UNSUPPORTED","Upgrade target unsupported"},
  {"INSUFFICIENT_STORAGE","Insufficient firmware storage"},{"DSP_TRANSFER_FAILED","DSP transfer failed"},
  {"BMS_TRANSFER_FAILED","BMS transfer failed"},{"IMAGE_FORMAT_INVALID","Firmware format invalid"},
  {"TARGET_NOT_READY","Target not ready"},
  {"TARGET_RESPONSE_TIMEOUT","Target did not restart"},
  {"TARGET_VERSION_UNCONFIRMED","Upgrade unconfirmed"},
  {"DIAGNOSTIC_ONLY","Diagnostic finished"},
  {"TASK_REPLAY_BLOCKED","Upgrade already requested"}};
 for(size_t i=0;i<sizeof(errors)/sizeof(*errors);i++)if(!strcmp(code,errors[i].code))return errors[i].label;
 return "Upgrade failed";
}
static const char *upgrade_detail(const OtaSnapshot *ota){
 if(!strcmp(ota->stage,"boot_check"))return "Checking startup health";
 if(ota->state==OTA_STATE_FAILED&&!strcmp(ota->error_code,"DIAGNOSTIC_ONLY"))return "Packet 0 only; Boot held";
 if(ota->state==OTA_STATE_FAILED&&!strcmp(ota->error_code,"TARGET_VERSION_UNCONFIRMED"))return "Firmware readback unavailable";
 if(ota->state==OTA_STATE_FAILED)return "Check error code and retry";
 if(ota->state==OTA_STATE_SUCCEEDED && ota->target!=OTA_TARGET_ESP){
  if(!strcmp(ota->message,"Target transfer complete; new App responded"))return "Target response received";
  if(!strcmp(ota->message,"Target transfer complete; application responded"))return "Target response received";
  if(!strcmp(ota->message,"Target transfer complete; target response not observed"))return "Target response not observed";
  return "Target response not confirmed";
 }
 if(!strcmp(ota->message,"Entering direct target Boot (waiting for HR)"))return "Waiting for target Boot";
 if(!strcmp(ota->message,"Waiting for target to run the new App")||
    !strcmp(ota->message,"Waiting for target application response"))return "Waiting for target response";
 static const char *const details[]={"Send firmware from App or cloud","Preparing firmware upgrade",
  "Downloading firmware","Receiving firmware","Checking firmware image","Writing firmware image",
  "Cancelling firmware upgrade","Firmware upgrade cancelled","Restarting controller",
  "Controller update confirmed","Firmware upgrade failed","Previous firmware restored"};
 unsigned state=(unsigned)ota->state;
 return state<sizeof(details)/sizeof(*details)?details[state]:"Upgrade status unavailable";
}
void direct_ui_build(const direct_ui_state_t *u,const direct_ui_data_t *d,direct_ui_view_t *v){
 memset(v,0,sizeof(*v));v->selected=-1;snprintf(v->footer,sizeof(v->footer),"ESC Back");
 if(u->page==UI_BMS_PROTOCOL){
  bool zh=d->language_zh,csergy=d->bms_protocol==PANEL_BMS_CSERGY;
  bool received=full_battery_sample_valid(d),online=received && d->bms_online;
  const char *active=zh?": 当前":": Active";
  bool checking=d->bms_detection==PANEL_BMS_DETECT_RUNNING;
  const char *status=d->ota_busy?(zh?"升级中":"Updating"):
   checking?(zh?"正在识别":"Detecting"):
   online?(zh?"通信正常":"Connected"):
   received?(zh?"已断联":"Disconnected"):
   d->bms_no_reply?(zh?"无有效应答":"No valid reply"):(zh?"正在检测":"Checking");
  snprintf(v->title,sizeof(v->title),"%s",zh?"电池连接":"Battery connection");
  if(u->editing){
   snprintf(v->rows[0],sizeof(v->rows[0]),"BBJ (0x7C)%s",csergy?"":active);
   snprintf(v->rows[1],sizeof(v->rows[1]),"CSERGY Modbus%s",csergy?active:"");
  }else{
   snprintf(v->rows[0],sizeof(v->rows[0]),"%s",zh?"自动识别":"Auto detect");
   snprintf(v->rows[1],sizeof(v->rows[1]),"%s",zh?"手动设置":"Manual setup");
   snprintf(v->rows[2],sizeof(v->rows[2]),"%s",zh?"充电与保护设置":"Charge and protection");
  }
  snprintf(v->rows[3],sizeof(v->rows[3]),"%s: %s",zh?"电池通信":"Battery",status);
  const char *result=u->message;
  if(!result[0] && !u->editing){
   if(d->bms_detection==PANEL_BMS_DETECT_MATCHED)result=zh?"识别成功":"Battery detected";
   else if(d->bms_detection==PANEL_BMS_DETECT_MISSING)result=zh?"未识别，请检查接线":"Check battery and wiring";
   else if(d->bms_detection==PANEL_BMS_DETECT_SAVE_FAILED)result="Save failed / retry";
  }
  snprintf(v->rows[4],sizeof(v->rows[4]),"%s",result);
  v->selected=u->selected;
  return;
 }
 if(u->page==UI_BMS){
  bool zh=d->language_zh;
  unsigned page=full_battery_page(u,d),count=full_battery_pages(d);
  snprintf(v->title,sizeof(v->title),zh?"电池状态 %u/%u":"Battery status %u/%u",page+1,count);
  if(!full_battery_sample_valid(d)){
   snprintf(v->rows[0],sizeof(v->rows[0]),"%s",zh?"电池未连接":"Battery disconnected");
   snprintf(v->rows[2],sizeof(v->rows[2]),"%s",zh?"检查电池供电与通信连接":"Check battery power and cable");
   snprintf(v->rows[4],sizeof(v->rows[4]),"%s",zh?"确认键连接电池":"Press ENTER to connect battery");
  }else{
   const char *name=d->bms_protocol==PANEL_BMS_CSERGY?"CSERGY":"BBJ";
   snprintf(v->rows[0],sizeof(v->rows[0]),"%s · %s",name,d->bms_online?(zh?"电池已连接":"Battery connected"):
    (zh?"已断联 · 上次数据":"Last data"));
  if(full_battery_cells(u,d)){
   unsigned start=(page-1)*8;
   for(unsigned i=0;i<8;i++){
    uint16_t mv=d->bms.cell_mv[start+i];
    if(mv && mv<=8191U)snprintf(v->rows[i+1],sizeof(v->rows[i+1]),zh?"电芯 %02u: %u mV":"Cell %02u: %u mV",start+i+1,mv);
    else snprintf(v->rows[i+1],sizeof(v->rows[i+1]),zh?"电芯 %02u: --":"Cell %02u: --",start+i+1);
   }
  }else{
   snprintf(v->rows[1],sizeof(v->rows[1]),zh?"电池电压: %u.%02u V":"Pack voltage: %u.%02u V",
    (unsigned)(d->bms.voltage_10mv/100),(unsigned)(d->bms.voltage_10mv%100));
   int64_t current=d->bms.current_10ma,magnitude=current<0?-current:current;
   snprintf(v->rows[2],sizeof(v->rows[2]),zh?"电池电流: %s%lu.%02lu A":"Pack current: %s%lu.%02lu A",current<0?"-":"",(unsigned long)(magnitude/100),(unsigned long)(magnitude%100));
   snprintf(v->rows[3],sizeof(v->rows[3]),"SOC: %u%%",direct_bms_percent(&d->bms,d->bms.soc_percent));
   snprintf(v->rows[4],sizeof(v->rows[4]),"SOH: %u%%",direct_bms_percent(&d->bms,d->bms.soh_percent));
   unsigned min=65535,max=0,count=0;
   for(unsigned i=0;i<8*full_battery_cell_pages(d);i++)if(d->bms.cell_mv[i] && d->bms.cell_mv[i]<=8191U){unsigned mv=d->bms.cell_mv[i];if(mv<min)min=mv;if(mv>max)max=mv;count++;}
   if(count){
    snprintf(v->rows[5],sizeof(v->rows[5]),zh?"最低单体: %u mV":"Minimum cell: %u mV",min);
    snprintf(v->rows[6],sizeof(v->rows[6]),zh?"最高单体: %u mV":"Maximum cell: %u mV",max);
    snprintf(v->rows[7],sizeof(v->rows[7]),zh?"单体压差: %u mV":"Cell difference: %u mV",max-min);
   }else snprintf(v->rows[5],sizeof(v->rows[5]),"%s",zh?"单体电压: --":"Cell voltages: --");
   snprintf(v->rows[8],sizeof(v->rows[8]),"%s",zh?"左右键查看单体电压":"Left / right for cell voltages");
  }
  }
 }else if(u->page==UI_PRODUCTION){
  snprintf(v->title,sizeof(v->title),"Manufacturing");
  snprintf(v->rows[0],sizeof(v->rows[0]),"%s",d->factory_ready?"Identity verified":"Awaiting provisioning");
  snprintf(v->rows[1],sizeof(v->rows[1]),"%s",d->model?d->model:"CS-MB-PANEL");
  snprintf(v->rows[2],sizeof(v->rows[2]),"SN: %s",d->sn&&d->sn[0]?d->sn:"--");
  snprintf(v->rows[3],sizeof(v->rows[3]),"Hardware: %s",d->hardware?d->hardware:"UNSET");
  snprintf(v->rows[4],sizeof(v->rows[4]),"%s",d->factory_locked?"Identity locked":"Identity unlocked");
  snprintf(v->rows[5],sizeof(v->rows[5]),"%s",d->factory_write_enabled?"USB provisioning enabled":"USB inspection only");
  snprintf(v->rows[6],sizeof(v->rows[6]),"Keys: %02lX / 3F",(unsigned long)d->factory_keys);
  snprintf(v->rows[7],sizeof(v->rows[7]),"Use USB service for provisioning");
  snprintf(v->footer,sizeof(v->footer),"ESC Back");
 }else if(u->page==UI_UPGRADE){
  snprintf(v->title,sizeof(v->title),"Firmware upgrade");
  static const char *const states[]={"Ready","Accepted","Downloading","Receiving","Verifying","Installing","Cancelling","Cancelled","Restarting","Succeeded","Failed","Rolled back"};
  /* 阶段名与 App 一致(downloading/transferring/verifying/installing...), 本机屏幕
   * 与手机看到同一格; 旧固件用状态名, 直连升级时只能显示笼统的 "Installing"。 */
  static const struct {const char *stage;const char *label;} stage_labels[]={
   {"accepted","Accepted"},{"downloading","Downloading"},{"verifying","Verifying"},
   {"transferring","Transferring"},{"installing","Installing"},
   {"boot_check","Confirming startup"},
   {"rebooting","Restarting"},{"succeeded","Succeeded"},{"failed","Failed"}};
  unsigned state=(unsigned)d->ota.state;
  const char *label=NULL;
  if(d->ota_busy && state!=OTA_STATE_CANCELLING)
   for(size_t i=0;i<sizeof(stage_labels)/sizeof(stage_labels[0]);++i)
    if(!strcmp(d->ota.stage,stage_labels[i].stage)){label=stage_labels[i].label;break;}
  if(!label)label=state<sizeof(states)/sizeof(states[0])?states[state]:"Unknown";
  if(state==OTA_STATE_SUCCEEDED && d->ota.target!=OTA_TARGET_ESP)label="Transfer complete";
  snprintf(v->rows[0],sizeof(v->rows[0]),"%s",label);
  snprintf(v->rows[1],sizeof(v->rows[1]),"%s / %s",d->ota.source==OTA_SOURCE_LOCAL?"Bluetooth":"Network",d->ota.target==OTA_TARGET_ESP?"Communication":d->ota.target==OTA_TARGET_ARM?"ARM":d->ota.target==OTA_TARGET_DSP?"DSP":d->ota.target==OTA_TARGET_BMS?"BMS":"--");
  if(d->ota.total)snprintf(v->rows[2],sizeof(v->rows[2]),"%u%%  %lu / %lu B",d->ota.progress,(unsigned long)d->ota.received,(unsigned long)d->ota.total);
  else snprintf(v->rows[2],sizeof(v->rows[2]),"%u%%",d->ota.progress);
  snprintf(v->rows[3],sizeof(v->rows[3]),"%s",d->ota.version);
  snprintf(v->rows[4],sizeof(v->rows[4]),"%s",state==OTA_STATE_FAILED?upgrade_error(d->ota.error_code):d->ota_busy?"Keep device powered on":"Firmware upgrade");
  if(state==OTA_STATE_FAILED && d->ota.error_code[0] && strcmp(d->ota.error_code,"OK"))
   snprintf(v->rows[5],sizeof(v->rows[5]),"%s",d->ota.error_code);
  snprintf(v->rows[6],sizeof(v->rows[6]),"%s",upgrade_detail(&d->ota));
  bool cancel=ota_cancellable(d);
  if(u->confirm && cancel)snprintf(v->rows[6],sizeof(v->rows[6]),"Stop this firmware transfer?");
  else if(u->message[0])snprintf(v->rows[6],sizeof(v->rows[6]),"%s",u->message);
  snprintf(v->footer,sizeof(v->footer),"%s",cancel?
   (u->confirm?"ENTER Confirm  ESC Back":"ENTER Cancel"):
   d->ota_busy?"Keep device powered on":"ESC Back");
 }else if(u->page==UI_HELP){
  snprintf(v->title,sizeof(v->title),"Device information");
  snprintf(v->rows[0],sizeof(v->rows[0]),"%s",d->model?d->model:"CS-MB-PANEL");snprintf(v->rows[1],sizeof(v->rows[1]),"Hardware: %s",d->hardware?d->hardware:"UNSET");
  snprintf(v->rows[2],sizeof(v->rows[2]),"Firmware: %s",d->version?d->version:"");snprintf(v->rows[3],sizeof(v->rows[3]),"SN: %s",d->sn?d->sn:"--");
  snprintf(v->rows[4],sizeof(v->rows[4]),"Wi-Fi: %s",d->wifi_connected?"Connected":"Disconnected");
  snprintf(v->rows[5],sizeof(v->rows[5]),"Cloud: %s",d->network_online?"Connected":"Disconnected");
 }else if(u->page==UI_TIME){
  const char *names[]={"Year","Month","Day","Hour","Minute","Second","Time zone"};snprintf(v->title,sizeof(v->title),"Time set");
  for(unsigned r=0;r<7;r++)snprintf(v->rows[r],sizeof(v->rows[r]),"%s: %d",names[r],u->time_fields[r]);
  int zone=u->time_fields[6],magnitude=zone<0?-zone:zone;
  snprintf(v->rows[6],sizeof(v->rows[6]),"Time zone: UTC%c%02d:%02d",zone<0?'-':'+',magnitude/60,magnitude%60);
  snprintf(v->rows[7],sizeof(v->rows[7]),"%s",u->editing?"Sync time after restart":"Time zone changes display only");
  v->selected=u->selected;
  snprintf(v->footer,sizeof(v->footer),"UP/DN Select  ENTER Save");
 }else if(u->page==UI_APPEARANCE){
  snprintf(v->title,sizeof(v->title),"Appearance");
  snprintf(v->rows[0],sizeof(v->rows[0]),"Theme: %s",u->appearance_theme?"Light":"Dark");
  snprintf(v->rows[1],sizeof(v->rows[1]),"Language: %s",u->appearance_language?"Chinese":"English");
  panel_display_settings_t display=appearance_display(u);
  snprintf(v->rows[2],sizeof(v->rows[2]),d->language_zh?"背光亮度: %u%%":"Backlight: %u%%",display.brightness);
  unsigned seconds=panel_display_timeout_ms(display)/1000U;
  if(seconds)snprintf(v->rows[3],sizeof(v->rows[3]),d->language_zh?"自动息屏: %u 秒":"Screen timeout: %u s",seconds);
  else snprintf(v->rows[3],sizeof(v->rows[3]),"%s",d->language_zh?"自动息屏: 永不":"Screen timeout: Never");
  snprintf(v->rows[4],sizeof(v->rows[4]),d->language_zh?"指示灯亮度: %u%%":"Status LED: %u%%",((unsigned)(uint8_t)u->time_fields[2]*100+127)/255);
  static const char *const loudness[][2]={{"Off","关闭"},{"Low","低"},{"Medium","中"},{"High","高"}};
  unsigned beep=(unsigned)u->time_fields[3]%PANEL_BEEP_COUNT;
  snprintf(v->rows[5],sizeof(v->rows[5]),d->language_zh?"蜂鸣器响度: %s":"Beep volume: %s",loudness[beep][d->language_zh?1:0]);
  snprintf(v->rows[6],sizeof(v->rows[6]),"Save appearance");v->selected=u->selected;
 }else if(u->page==UI_FACTORY){
  snprintf(v->title,sizeof(v->title),"%s",u->group==2?"LCD reset":u->group==1?"Restart controller":"Reset local UI settings");
  snprintf(v->rows[1],sizeof(v->rows[1]),"%s",u->group==2?"Reset LCD only?":u->group==1?"Restart communication?":"Reset display, sound and time?");
  snprintf(v->rows[3],sizeof(v->rows[3]),"%s",u->confirm?"YES selected":"NO selected");
  snprintf(v->rows[5],sizeof(v->rows[5]),"Inverter settings retained");snprintf(v->footer,sizeof(v->footer),"L/R Yes/No  ENTER Confirm  ESC Back");
 }
 if(u->message[0])snprintf(v->footer,sizeof(v->footer),"%s",u->message);
}

