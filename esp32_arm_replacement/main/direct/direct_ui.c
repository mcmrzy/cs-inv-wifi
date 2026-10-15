#include "direct_ui.h"
#include "direct_ui_assets.h"
#include "direct_ui_locale.h"
#include "direct_gfx_assets.h"
#include <stdio.h>
#include <string.h>

/* CollectorParamAddr.h indices; no ARM RAM layout is reused for UI state. */
typedef struct { const char *name; uint16_t min, max, step; } parameter_t;
#define P(n,a,b,s) {n,a,b,s}
static const parameter_t params[CONFIG_PARAM_MAX_INDEX] = {
 P("Output source",1,4,1), P("Max charge A",0,140,1), P("AC input range",1,3,1),
 P("Battery capacity Ah",50,2000,50), P("Battery type",1,5,1),
 P("Overload restart",0,1,1), P("Overtemp restart",0,1,1), P("Output voltage dV",2080,2400,10),
 P("Output frequency",5000,6000,1000), P("Master/slave",0,1,1), P("AC max charge A",0,60,1),
 P("Back to utility dV",440,512,1), P("Back to battery dV",480,580,1),
 P("Charge source",1,4,1), P("Alarm control",0,1,1), P("Screen blank",0,1,1),
 P("Source lost alarm",0,1,1), P("Overload bypass",0,1,1),
 P("Parallel discharge A",0,1000,1), P("Parallel charge A",0,1000,1),
 P("Cutoff recover dV",0,100,1), P("Solar power balance",0,1,1),
 P("Output mode",0,4,1), P("Lithium material",0,1,1), P("LiFePO4 cells",4,32,1),
 P("Li-NMC cells",4,32,1), P("Equalization enable",0,1,1),
 P("Equalization dV",540,584,1), P("Equalization min",5,900,5),
 P("Eq timeout min",5,900,5), P("Eq interval days",1,90,1), P("Eq activate",0,1,1),
 P("AC charge start hour",0,23,1), P("AC charge end hour",0,23,1),
 P("Generator start dV",400,580,1), P("Generator stop dV",400,584,1),
 P("Utility start SOC",0,100,1), P("Battery return SOC",0,100,1),
 P("Generator start SOC",0,100,1), P("Generator stop SOC",0,100,1),
 P("Cutoff SOC",5,30,1), P("Buzzer",0,1,1), P("Socket S SOC",0,100,1),
 P("Socket A SOC",0,100,1), P("Socket B SOC",0,100,1),
 P("Manual socket",0,65535,1), P("Control socket",0,65535,1),
 P("SOC recover gap",0,100,1), P("BMS protocol",1,15,1),
 P("Generator power W",0,20000,100), P("Generator balance",0,1,1),
 P("Gen start1 low",0,65535,1), P("Gen start1 high",0,65535,1),
 P("Gen end1 low",0,65535,1), P("Gen end1 high",0,65535,1),
 P("Gen start2 low",0,65535,1), P("Gen start2 high",0,65535,1),
 P("Gen end2 low",0,65535,1), P("Gen end2 high",0,65535,1),
 P("Output start1 low",0,65535,1), P("Output start1 high",0,65535,1),
 P("Output end1 low",0,65535,1), P("Output end1 high",0,65535,1),
 P("Output start2 low",0,65535,1), P("Output start2 high",0,65535,1),
 P("Output end2 low",0,65535,1), P("Output end2 high",0,65535,1),
 P("AGM bulk dV",480,584,1), P("Flooded bulk dV",480,584,1),
 P("User Pb bulk dV",480,584,1), P("AGM float dV",480,584,1),
 P("Flooded float dV",480,584,1), P("Lithium float dV",400,600,1),
 P("User Pb float dV",480,584,1), P("User LFP float dV",400,600,1),
 P("User NMC float dV",400,600,1), P("AGM cutoff dV",400,480,1),
 P("Flooded cutoff dV",400,480,1), P("Lithium cutoff dV",300,600,1),
 P("User Pb cutoff dV",400,480,1), P("User LFP cutoff dV",300,600,1),
 P("User NMC cutoff dV",300,600,1), P("AC charge max SOC",0,100,1),
 P("AC charge max dV",480,584,1)
};
#undef P
static const uint8_t input[] = {2};
static const uint8_t output[] = {0,22,9,12,11,37,36,7,8,20,47,59,60,61,62,63,64,65,66};
static const uint8_t charging[] = {13,1,10,82,83,32,33,21};
static const uint8_t battery[] = {4,48,23,24,25,3,40,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81};
static const uint8_t generator[] = {49,34,35,38,39,50,51,52,53,54,55,56,57,58};
static const uint8_t other[] = {5,6,14,16,17,41,15,18,19};
static const uint8_t equalization[] = {26,27,28,29,30,31};
static const char *const menus[] = {"Input / Output", "Charging", "Battery / EQ", "Generator", "System", "Advanced"};
static const char *const sub0[]={"Input set","Output set"};
static const char *const sub2[]={"Battery set","Equalization"};
static const char *const sub3[]={"Generator set","Generator information"};
static const char *const sub4[]={"Other set","Appearance","Time set","Device information","Restart controller","Reset local UI settings","LCD reset","Firmware upgrade","Manufacturing","Phone connection"};
static const char *const *const submenus[]={sub0,NULL,sub2,sub3,sub4,NULL};
static const uint8_t subcounts[]={2,0,2,2,10,0};
unsigned direct_ui_submenu_count(unsigned menu){return menu<6?subcounts[menu]:0;}
const char *direct_ui_submenu_name(unsigned menu,unsigned position){return position<direct_ui_submenu_count(menu)?submenus[menu][position]:NULL;}
static const uint8_t *const groups[] = {input, output, charging, battery, generator, other, equalization};
static const uint8_t counts[] = {sizeof(input),sizeof(output),sizeof(charging),sizeof(battery),
 sizeof(generator),sizeof(other),sizeof(equalization),CONFIG_PARAM_MAX_INDEX};
static const char *const group_names[] = {"Input set","Output set","Charging set","Battery set",
 "Generator set","Other set","Equalization","All parameters"};
static unsigned item_index(const direct_ui_state_t *u) {
 return u->group == 7 ? u->selected : groups[u->group][u->selected];
}
const char *direct_ui_param_name(unsigned index) { return index < CONFIG_PARAM_MAX_INDEX ? params[index].name : "Unknown"; }
unsigned direct_ui_menu_count(void) { return (unsigned)(sizeof(menus)/sizeof(menus[0])); }
const char *direct_ui_menu_name(unsigned index) { return index<direct_ui_menu_count()?menus[index]:NULL; }
unsigned direct_ui_group_count(unsigned group) { return group<sizeof(counts)/sizeof(counts[0])?counts[group]:0; }
int direct_ui_group_param(unsigned group,unsigned position) {
 if(position>=direct_ui_group_count(group))return -1;
 return group==7?(int)position:(int)groups[group][position];
}
bool direct_ui_value_allowed(unsigned index, uint16_t value) {
 if(index==8)return value==5000 || value==6000;
 if(index==13)return value==1 || value==3 || value==4;
 return index < CONFIG_PARAM_MAX_INDEX && value >= params[index].min && value <= params[index].max;
}
static void note(direct_ui_state_t *u, const char *message) { snprintf(u->message,sizeof(u->message),"%s",message); u->dirty=true; }
/* Screen copy is independent of protocol diagnostics. Never translate or alter
 * the snapshot: MQTT/BLE retain the original error code and detailed message. */
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
void direct_ui_init(direct_ui_state_t *u) { memset(u,0,sizeof(*u)); u->dirty=true; }
bool direct_ui_has_alarm(const direct_ui_data_t *d) {
 if(!d)return false;
 return (d->dsp_online && (d->run.FaultValue || d->run.Warning || d->run.BmsWarning)) ||
  (d->status_valid && (d->status.fault || d->status.warning[0] || d->status.warning[1] || d->status.warning[2])) ||
  (d->bms_online && (d->bms.warning || d->bms.protection));
}
void direct_ui_write_result(direct_ui_state_t *u, int result) {
 if (!u->pending || result==1) return;
 u->pending=false;
 if(result==0 || result==2){u->editing=false;u->page=UI_CONFIG;}else{u->editing=true;u->page=UI_REVIEW;}
 note(u,result==2 ? "Saved on ESP controller" : result==0 ? "Saved / confirmed" : result==-1 ? "No confirmation / timeout" : "Save rejected");
}
static unsigned wrap(unsigned n, int delta, unsigned count) { return (unsigned)((int)n+delta+(int)count)%count; }
static unsigned alarm_count(const direct_ui_data_t *d) {
 uint64_t warnings=d->status_valid?d->status.warning[0]|((uint64_t)d->status.warning[1]<<16)|((uint64_t)d->status.warning[2]<<32):0;
 uint32_t faults=d->status_valid?d->status.fault:0,bms=d->bms_online?d->bms.warning|((uint32_t)d->bms.protection<<16):0;
 unsigned count=0;for(;warnings;warnings&=warnings-1)count++;
 for(;faults;faults&=faults-1)count++;
 for(;bms;bms&=bms-1)count++;
 return count;
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
static bool editable(direct_ui_state_t *u,unsigned i,const direct_ui_data_t *d,direct_ui_event_t *e) {
 if(i>=CONFIG_PARAM_MAX_INDEX){note(u,"Value outside allowed range");return false;}
 /* Controller-owned writable settings can be saved during a DSP outage. */
 if(!d->valid[i]){note(u,"Value unknown / refresh DSP");e->action=UI_ACT_REFRESH;return false;}
 if(!d->writable[i]){note(u,"Read only on this board");return false;}
 return true;
}
static void enter_group(direct_ui_state_t *u,unsigned group,direct_ui_page_t parent){
 u->group=(uint8_t)group;u->selected=u->group_selected[group];u->parent_page=parent;u->page=UI_CONFIG;u->editing=false;
}
static void return_settings(direct_ui_state_t *u){
 u->page=u->parent_page==UI_SUBMENU?UI_SUBMENU:UI_MENU;
 u->selected=u->page==UI_SUBMENU?u->submenu_selected:u->menu_selected;
}
static bool quick_page(direct_ui_page_t page){return page==UI_HOME || page==UI_BMS || page==UI_PARAMETERS || page==UI_ALARM;}
static unsigned quick_index(direct_ui_page_t page){return page==UI_BMS?1:page==UI_PARAMETERS?2:page==UI_ALARM?3:0;}
static void save_quick(direct_ui_state_t *u){unsigned i=quick_index(u->page);u->quick_detail[i]=u->detail_page;u->quick_selected[i]=u->selected;}
static void restore_quick(direct_ui_state_t *u){unsigned i=quick_index(u->page);u->detail_page=u->quick_detail[i];u->selected=u->quick_selected[i];}
static uint8_t page_step(unsigned selected,int delta,unsigned count,unsigned rows){
 if(!count)return 0;
 unsigned page=wrap(selected/rows,delta,(count+rows-1)/rows),next=page*rows+selected%rows;
 return (uint8_t)(next<count?next:count-1);
}
void direct_ui_key_hints(const direct_ui_state_t *u,const direct_ui_data_t *d,const char *h[6]){
 for(unsigned i=0;i<6;i++)h[i]="--";
 if(u->pending)return;
 h[0]="Back";
 if(d->ota_busy && (u->page==UI_CONFIG || u->page==UI_REVIEW || u->page==UI_FACTORY || u->page==UI_TIME || u->page==UI_APPEARANCE))return;
 if(quick_page(u->page) && u->parent_page!=UI_SUBMENU){
  h[0]=u->page==UI_HOME?(u->main_mode?"Overview":"--"):"Home";
  h[1]="Prev";h[4]="Next";h[5]="Menu";
  if(u->page==UI_HOME)h[2]=h[3]=u->main_mode?"Overview":"Details";
  else if(u->page==UI_BMS)h[2]=h[3]=u->detail_page?"Summary":"Cells";
  else{h[2]="Up";h[3]="Down";}
 }else if(u->page==UI_MENU){h[1]="Left";h[2]="Up";h[3]="Down";h[4]="Right";h[5]="Open";}
 else if(u->page==UI_SUBMENU || (u->page==UI_CONFIG && !u->editing)){
  h[1]="Prev";h[2]="Up";h[3]="Down";h[4]="Next";h[5]=u->page==UI_SUBMENU?"Open":"Edit";
 }else if(u->page==UI_CONFIG){
  h[0]="Cancel";h[1]="-";h[4]="+";h[2]="Fast+";h[3]="Fast-";h[5]="Review";
 }else if(u->page==UI_REVIEW){h[0]="Edit";h[5]="Confirm";}
 else if(u->page==UI_FACTORY){h[1]="NO";h[4]="YES";if(u->confirm)h[5]="Confirm";}
 else if(u->page==UI_TIME){h[1]="-";h[2]="Up";h[3]="Down";h[4]="+";h[5]="Save";}
 else if(u->page==UI_APPEARANCE){h[0]="Cancel";h[2]="Up";h[3]="Down";h[5]=u->selected==2?"Save":"Change";if(u->selected<2)h[1]=h[4]="Change";}
 else if(u->page==UI_HELP) {h[1]="Prev";h[4]="Next";}
 else if(u->page==UI_UPGRADE && d->ota_busy && (d->ota.state==OTA_STATE_ACCEPTED || d->ota.state==OTA_STATE_DOWNLOADING || d->ota.state==OTA_STATE_RECEIVING))h[5]="Cancel";
}
direct_ui_event_t direct_ui_key(direct_ui_state_t *u,uint8_t key,uint32_t now,const direct_ui_data_t *d){
 direct_ui_event_t e={0};(void)now;
 if(d->ota_busy && (u->page==UI_CONFIG || u->page==UI_REVIEW || u->page==UI_FACTORY || u->page==UI_TIME || u->page==UI_APPEARANCE) && key!=1){note(u,"Upgrade busy");return e;}
 if(u->page==UI_UPGRADE){
  if(key==1)return_settings(u);
  else if(key==2 && d->ota_busy && (d->ota.state==OTA_STATE_ACCEPTED || d->ota.state==OTA_STATE_DOWNLOADING || d->ota.state==OTA_STATE_RECEIVING))e.action=UI_ACT_OTA_CANCEL;
  return e;
 }
 if(!key || key==7){u->combo_count=0;return e;}
 u->dirty=true;
 if(u->pending){note(u,"Sending settings");return e;}
 u->message[0]=0;
 if(quick_page(u->page) && u->parent_page!=UI_SUBMENU){
  static const direct_ui_page_t pages[]={UI_HOME,UI_BMS,UI_PARAMETERS,UI_ALARM};
  unsigned current=0;for(unsigned i=0;i<4;i++)if(pages[i]==u->page)current=i;
  if(key==4 || key==6){save_quick(u);u->page=pages[wrap(current,key==4?-1:1,4)];restore_quick(u);}
  else if(key==1){save_quick(u);u->page=UI_HOME;restore_quick(u);u->main_mode=0;}
  else if(key==2){
   save_quick(u);u->menu_origin=u->page;u->page=UI_MENU;u->selected=u->menu_selected;e.action=UI_ACT_REFRESH;
  }else if(key==3 || key==5){
   int delta=key==3?-1:1;
   if(u->page==UI_BMS)u->detail_page=(uint8_t)wrap(u->detail_page,delta,2);
   else if(u->page==UI_PARAMETERS)u->detail_page=(uint8_t)wrap(u->detail_page,delta,DIRECT_UI_PARAMETER_PAGES);
   else if(u->page==UI_ALARM){unsigned count=alarm_count(d);u->selected=(uint8_t)wrap(u->selected,delta,count?count:1);}
   else u->main_mode^=1;
  }
 }else if(u->page==UI_MENU){
  if(key==1){u->page=quick_page(u->menu_origin)?u->menu_origin:UI_HOME;u->parent_page=UI_HOME;restore_quick(u);}
  else if(key==3 || key==5 || key==4 || key==6){
   unsigned row=u->selected/2,col=u->selected%2;
   if(key==3 || key==5)row=wrap(row,key==3?-1:1,3);else col=key==4?0:1;
   u->selected=(uint8_t)(row*2+col);u->menu_selected=u->selected;
  }
  else if(key==2){
   u->menu_selected=u->selected;u->submenu_selected=0;
   if(u->selected==1)enter_group(u,2,UI_MENU);
   else if(u->selected==5)enter_group(u,7,UI_MENU);
   else{u->page=UI_SUBMENU;u->selected=0;}
   e.action=UI_ACT_REFRESH;
  }
 }else if(u->page==UI_SUBMENU){
  if(key==1){u->page=UI_MENU;u->selected=u->menu_selected;u->parent_page=UI_HOME;}
  else if(key==3 || key==5 || key==4 || key==6){
   unsigned count=direct_ui_submenu_count(u->menu_selected);
   u->selected=key==4 || key==6?page_step(u->selected,key==4?-1:1,count,DIRECT_UI_SUBMENU_ROWS):(uint8_t)wrap(u->selected,key==3?-1:1,count);
   u->submenu_selected=u->selected;
  }
  else if(key==2){
   unsigned m=u->menu_selected,n=u->selected;u->submenu_selected=(uint8_t)n;u->parent_page=UI_SUBMENU;
   if(m==0)enter_group(u,n,UI_SUBMENU);
   else if(m==2)enter_group(u,n?6:3,UI_SUBMENU);
   else if(m==3){if(!n)enter_group(u,4,UI_SUBMENU);else{u->page=UI_PARAMETERS;u->detail_page=4;}}
   else if(m==4){
    u->selected=0;
    if(!n)enter_group(u,5,UI_SUBMENU);
    else if(n==1){u->page=UI_APPEARANCE;u->appearance_theme=d->theme_light;u->appearance_language=d->language_zh;}
    else if(n==2){u->page=UI_TIME;int fields[]={d->year,d->month,d->day,d->hour,d->minute,d->second,d->timezone_minutes};memcpy(u->time_fields,fields,sizeof(fields));}
    else if(n==3){u->page=UI_HELP;u->detail_page=0;}
    else if(n==7){u->page=UI_UPGRADE;u->detail_page=0;}
    else if(n==8){u->page=UI_PRODUCTION;u->detail_page=0;}
    else if(n==9){u->page=UI_PHONE_SETUP;u->detail_page=0;}
    else{u->page=UI_FACTORY;u->group=n==4?1:n==5?0:2;u->confirm=false;}
   }e.action=UI_ACT_REFRESH;
  }
 }else if(u->page==UI_CONFIG || u->page==UI_REVIEW){
  unsigned index=item_index(u);
  if(key==1){if(u->page==UI_REVIEW)u->page=UI_CONFIG;else if(u->editing)u->editing=false;else{u->group_selected[u->group]=u->selected;return_settings(u);}}
  else if(u->page==UI_CONFIG && !u->editing && (key==3 || key==5)){u->selected=(uint8_t)wrap(u->selected,key==3?-1:1,counts[u->group]);u->group_selected[u->group]=u->selected;}
  else if(u->page==UI_CONFIG && !u->editing && (key==4 || key==6)){u->selected=page_step(u->selected,key==4?-1:1,counts[u->group],DIRECT_UI_CONFIG_ROWS);u->group_selected[u->group]=u->selected;}
  else if(u->page==UI_CONFIG && u->editing && (key==3 || key==5 || key==4 || key==6)){
   int step=params[index].step;
   bool enumeration=params[index].max<=5 || index==8 || index==48;
   if((key==3 || key==5) && !enumeration)step*=10;
   int direction=key==4 || key==5?-1:1;
   int value=(int)u->draft+direction*step;
   if(index==13 && value==2)value+=direction;
   if(value<params[index].min)value=params[index].min;
   if(value>params[index].max)value=params[index].max;
   u->draft=(uint16_t)value;
  }else if(key==2){
   if(!editable(u,index,d,&e))return e;
   if(!u->editing){u->draft=d->config[index];u->editing=true;}
   else if(u->page==UI_CONFIG){u->page=UI_REVIEW;}
   else{
    if(!direct_ui_value_allowed(index,u->draft)){note(u,"Value outside allowed range");return e;}
    if((index==26 || index==31) && (!d->valid[4] || (d->config[4]!=2 && d->config[4]!=3))){note(u,"Equalization: lead acid only");return e;}
    e.action=UI_ACT_WRITE;e.index=(uint16_t)index;e.value=u->draft;u->pending=true;note(u,"Saving user settings");
   }
  }
 }else if(u->page==UI_APPEARANCE){
  if(key==1){return_settings(u);e.action=UI_ACT_APPEARANCE_CANCEL;}
  else if(key==3 || key==5)u->selected=(uint8_t)wrap(u->selected,key==3?-1:1,3);
  else if((key==4 || key==6 || key==2) && u->selected<2){if(!u->selected)u->appearance_theme=!u->appearance_theme;else u->appearance_language=!u->appearance_language;e.action=UI_ACT_APPEARANCE_PREVIEW;e.value=(uint16_t)(u->appearance_theme | (u->appearance_language<<1));}
  else if(key==2){e.action=UI_ACT_APPEARANCE_SAVE;e.value=(uint16_t)(u->appearance_theme | (u->appearance_language<<1));}
 }else if(u->page==UI_FACTORY){
  if(key==1){return_settings(u);u->confirm=false;}
  else if(key==4 || key==6)u->confirm=key==6;
  else if(key==2 && u->confirm)e.action=u->group==2?UI_ACT_LCD_RESET:u->group==1?UI_ACT_RESTART:UI_ACT_LOCAL_RESET;
 }else if(u->page==UI_TIME){
  static const int low[]={2020,1,1,0,0,0,-720},high[]={2099,12,31,23,59,59,840};
  if(key==1)return_settings(u);
  else if(key==3 || key==5)u->selected=(uint8_t)wrap(u->selected,key==3?-1:1,7);
  else if(key==4 || key==6){int i=u->selected;int v=u->time_fields[i]+(key==4?-1:1)*(i==6?30:1);if(v>=low[i] && v<=high[i])u->time_fields[i]=v;}
  else if(key==2){int64_t seconds;if(direct_ui_time_utc(u->time_fields,&seconds))e.action=UI_ACT_SET_TIME;else note(u,"Invalid calendar date");}
 }else if(key==1)return_settings(u);
 else if(u->page==UI_HELP && (key==4 || key==6))u->detail_page=(uint8_t)wrap(u->detail_page,key==4?-1:1,3);
 return e;
}
static void dec(char *buf,size_t size,int32_t value,unsigned scale) {
 uint32_t absval=value<0?(uint32_t)(-(int64_t)value):(uint32_t)value;
 if(scale==100)snprintf(buf,size,"%s%lu.%02lu",value<0?"-":"",(unsigned long)(absval/100),(unsigned long)(absval%100));
 else snprintf(buf,size,"%s%lu.%lu",value<0?"-":"",(unsigned long)(absval/10),(unsigned long)(absval%10));
}
static void row_num(direct_ui_view_t *v,int row,const char *label,int32_t value,unsigned scale,const char *unit) {
 char b[24];if(scale)dec(b,sizeof(b),value,scale);else snprintf(b,sizeof(b),"%ld",(long)value);
 snprintf(v->rows[row],sizeof(v->rows[row]),"%s: %s%s",label,b,unit);
}
void direct_ui_config_value(unsigned i, uint16_t val,char *b,size_t size) {
 if(!b || !size)return;
 if(i>=CONFIG_PARAM_MAX_INDEX){snprintf(b,size,"--");return;}
 static const char *const src[]={"-","Solar first","Utility first","S>B>U","S>U>B"};
 static const char *const chg[]={"-","Solar first","-","Solar+Utility","Only solar"};
 static const char *const range[]={"-","Appliance","UPS","Generator"};
 static const char *const types[]={"-","AGM","Flooded","User Pb","Lithium","User Li"};
 if(i==0 && val<5)snprintf(b,size,"%s",src[val]);
 else if(i==13 && val<5)snprintf(b,size,"%s",chg[val]);
 else if(i==2 && val<4)snprintf(b,size,"%s",range[val]);
 else if(i==4 && val<6)snprintf(b,size,"%s",types[val]);
 else if(i==8)snprintf(b,size,"%u.%02uHz",val/100,val%100);
 else if(params[i].max==1)snprintf(b,size,"%s",val?"On":"Off");
 else if(strstr(params[i].name,"dV"))snprintf(b,size,"%u.%uV",val/10,val%10);
 else if(strstr(params[i].name,"SOC"))snprintf(b,size,"%u%%",val);
 else if(strstr(params[i].name,"Ah"))snprintf(b,size,"%uAh",val);
 else if(strstr(params[i].name," A"))snprintf(b,size,"%uA",val);
 else if(strstr(params[i].name," W"))snprintf(b,size,"%uW",val);
 else if(strstr(params[i].name," min"))snprintf(b,size,"%umin",val);
 else if(strstr(params[i].name," days"))snprintf(b,size,"%ud",val);
 else if(strstr(params[i].name," hour"))snprintf(b,size,"%uh",val);
 else snprintf(b,size,"%u",val);
}
static unsigned alarm_lines(direct_ui_view_t *v,unsigned row,const char *text,unsigned limit) {
 unsigned used=0;
 while(*text && used<limit){
  unsigned length=0,pixels=0;
  while(text[length] && length<sizeof(v->rows[0])-1){
   const char *next=text+length;
   uint32_t codepoint=direct_gfx_utf8_next(&next);
   unsigned bytes=(unsigned)(next-(text+length));
   if(length+bytes>=sizeof(v->rows[0]))break;
   unsigned width=direct_gfx_glyph(codepoint,12)->advance;
   if(pixels+width>300)break;
   pixels+=width;length+=bytes;
  }
  if(!length)break;
  memcpy(v->rows[row+used],text,length);v->rows[row+used][length]=0;
  text+=length;while(*text==' ')text++;used++;
 }
 return used;
}
void direct_ui_build(const direct_ui_state_t *u,const direct_ui_data_t *d,direct_ui_view_t *v) {
 memset(v,0,sizeof(*v));v->selected=-1;
 snprintf(v->footer,sizeof(v->footer),"ESC Back  ENTER Menu");
 const RunParamDef *p=&d->run;
 if(u->page==UI_HOME){
  snprintf(v->title,sizeof(v->title),"%s  %s",d->network_online?"Network online":"Network offline",d->clock?d->clock:"");
  row_num(v,0,"AC input",p->GridVolt,0,"V");row_num(v,1,"AC frequency",p->GridFreq,10,"Hz");
  row_num(v,2,"PV power",p->Ppv,0,"W");row_num(v,3,"Battery",p->BatVolt,10,"V");
  row_num(v,4,"Battery SOC",p->BatterySOC,0,"%");row_num(v,5,"Load",p->OutputWatt,0,"W");
  row_num(v,6,"Output",p->ACOutputVolt,0,"V");row_num(v,7,"Temperature",p->InvertTemp,10,"C");
  if(d->status_valid)snprintf(v->rows[8],sizeof(v->rows[8]),"Fault %lu  Warning %04X",(unsigned long)d->status.fault,d->status.warning[1]);
  else snprintf(v->rows[8],sizeof(v->rows[8]),"DSP alarm status unavailable");
  snprintf(v->footer,sizeof(v->footer),"L/R Page  ENTER Set");
 }else if(u->page==UI_MENU){
  snprintf(v->title,sizeof(v->title),"System menu");unsigned start=u->selected/9*9;
  for(unsigned r=0;r<9 && start+r<sizeof(menus)/sizeof(menus[0]);r++)snprintf(v->rows[r],sizeof(v->rows[r]),"%u %s",start+r+1,menus[start+r]);
  v->selected=u->selected-start;
 }else if(u->page==UI_SUBMENU){
  snprintf(v->title,sizeof(v->title),"%s",menus[u->menu_selected]);
  unsigned start=u->selected/DIRECT_UI_SUBMENU_ROWS*DIRECT_UI_SUBMENU_ROWS;
  for(unsigned r=0;r<DIRECT_UI_SUBMENU_ROWS && start+r<direct_ui_submenu_count(u->menu_selected);r++)snprintf(v->rows[r],sizeof(v->rows[r]),"%s",direct_ui_submenu_name(u->menu_selected,start+r));
  v->selected=u->selected-start;
 }else if(u->page==UI_REVIEW){
  unsigned index=item_index(u);char current[24],draft[24];
  direct_ui_config_value(index,d->config[index],current,sizeof(current));direct_ui_config_value(index,u->draft,draft,sizeof(draft));
  snprintf(v->title,sizeof(v->title),"Review change");
  snprintf(v->rows[0],sizeof(v->rows[0]),"%s",params[index].name);
  snprintf(v->rows[2],sizeof(v->rows[2]),"Current: %s",d->valid[index]?current:"--");
  snprintf(v->rows[4],sizeof(v->rows[4]),"Draft: %s",draft);
  snprintf(v->rows[6],sizeof(v->rows[6]),"ENTER Confirm  ESC Edit");
 }else if(u->page==UI_CONFIG){
  snprintf(v->title,sizeof(v->title),"%s",group_names[u->group]);unsigned start=u->selected/DIRECT_UI_CONFIG_ROWS*DIRECT_UI_CONFIG_ROWS;
  for(unsigned r=0;r<DIRECT_UI_CONFIG_ROWS && start+r<counts[u->group];r++){
   unsigned index=u->group==7?start+r:groups[u->group][start+r];char b[24];
   if(d->valid[index])direct_ui_config_value(index,u->editing && start+r==u->selected?u->draft:d->config[index],b,sizeof(b));else snprintf(b,sizeof(b),"--");
   snprintf(v->rows[r],sizeof(v->rows[r]),"%02X %s: %s%s",index,params[index].name,b,u->editing && start+r==u->selected?" *":"");
  }
  v->selected=u->selected-start;snprintf(v->footer,sizeof(v->footer),"UP/DN Select  ENTER Edit");
 }else if(u->page==UI_PARAMETERS){
  snprintf(v->title,sizeof(v->title),"Running parameters %u/%u",u->detail_page+1,DIRECT_UI_PARAMETER_PAGES);
  if(u->detail_page==0){
   if(d->status_valid || d->dsp_status_held)snprintf(v->rows[0],sizeof(v->rows[0]),"InvS %04X %04X %04X",d->status.inverter_status[0],d->status.inverter_status[1],d->status.inverter_status[2]);else snprintf(v->rows[0],sizeof(v->rows[0]),"InvS: --");
   row_num(v,1,"Grid current",p->ACChrCurr,100,"A");row_num(v,2,"PV1 voltage",p->Vpv1,0,"V");row_num(v,3,"PV1 current",p->Buck1Curr,100,"A");
   row_num(v,4,"PV2 voltage",p->Vpv2,0,"V");row_num(v,5,"PV2 current",p->Buck2Curr,100,"A");
   row_num(v,6,"Bus",p->BusVolt,10,"V");row_num(v,7,"Output current",p->OutputCurr,100,"A");row_num(v,8,"Load ratio",p->LoadPercent,10,"%");
  }else if(u->detail_page==1){
   row_num(v,0,"Inverter",p->VinvtA,0,"V");row_num(v,1,"Inverter current",p->IinvtA,100,"A");row_num(v,2,"Inverter freq",p->InvtFreq,10,"Hz");
   row_num(v,3,"Inverter power",p->Pinvt,0,"W");row_num(v,4,"Inverter apparent",p->Sinvt,0,"VA");
   row_num(v,5,"Battery charge",p->BatChgCurr,10,"A");row_num(v,6,"Battery discharge",p->BatDischgCurr,10,"A");row_num(v,7,"Load apparent",p->OutputVA,0,"VA");row_num(v,8,"Grid power",p->ACInWatt,0,"W");
  }else if(u->detail_page==2){
   const char *labels[]={"PV","AC in","AC out","Battery charge","Battery discharge","Output"};
   snprintf(v->rows[0],sizeof(v->rows[0]),"Today / Total kWh (ESP meter)");
   for(unsigned r=0;r<6;r++){
    uint64_t today=d->energy_today[r]/36000000ULL,total=d->energy_total[r]/36000000ULL;
    if(!d->energy_valid)snprintf(v->rows[r+1],sizeof(v->rows[r+1]),"%s: --",labels[r]);
    else if(!d->clock_valid)snprintf(v->rows[r+1],sizeof(v->rows[r+1]),"%s: -- / %lu.%02lu",labels[r],(unsigned long)(total/100),(unsigned long)(total%100));
    else snprintf(v->rows[r+1],sizeof(v->rows[r+1]),"%s: %lu.%02lu / %lu.%02lu",labels[r],(unsigned long)(today/100),(unsigned long)(today%100),(unsigned long)(total/100),(unsigned long)(total%100));
   }
   snprintf(v->rows[7],sizeof(v->rows[7]),"Generator: -- (no power channel)");
   snprintf(v->rows[8],sizeof(v->rows[8]),"Since ESP install / NVS checkpoint");
  }else if(u->detail_page==3){
   snprintf(v->rows[0],sizeof(v->rows[0]),"DSP %s",d->dsp_online?"online":"offline");
   snprintf(v->rows[1],sizeof(v->rows[1]),"Polls %lu  CRC errors %lu",(unsigned long)d->polls,(unsigned long)d->errors);
   snprintf(v->rows[2],sizeof(v->rows[2]),"BMS %s",d->bms_online?"online":"offline");
   if(d->status_valid || d->dsp_status_held)snprintf(v->rows[3],sizeof(v->rows[3]),"Mode %u",d->status.mode);
   else snprintf(v->rows[3],sizeof(v->rows[3]),"Mode -- (status unavailable)");
   row_num(v,4,"Bus current",p->Ibus,100,"A");row_num(v,5,"PV1 power",p->Ppv1,0,"W");row_num(v,6,"PV2 power",p->Ppv2,0,"W");
   snprintf(v->rows[7],sizeof(v->rows[7]),"ESP %s",d->version?d->version:"");snprintf(v->rows[8],sizeof(v->rows[8]),"Direct DSP / no ARM UART");
  }else{
   snprintf(v->rows[0],sizeof(v->rows[0]),"Generator monitoring only");
   if(d->status_valid || d->dsp_status_held){
    snprintf(v->rows[1],sizeof(v->rows[1]),"Generator input: %s",(d->status.inverter_status[0]&0x0002U)?"valid":"not valid");
    snprintf(v->rows[2],sizeof(v->rows[2]),"Utility input: %s",(d->status.inverter_status[0]&0x0001U)?"valid":"not valid");
    snprintf(v->rows[3],sizeof(v->rows[3]),"Mode %u",d->status.mode);
   }else{
    snprintf(v->rows[1],sizeof(v->rows[1]),"Source status unavailable");
    snprintf(v->rows[3],sizeof(v->rows[3]),"Mode --");
   }
   snprintf(v->rows[4],sizeof(v->rows[4]),"Generator power: --");
   snprintf(v->rows[5],sizeof(v->rows[5]),"Generator energy: --");
   snprintf(v->rows[6],sizeof(v->rows[6]),"No independent power channel");
   snprintf(v->rows[7],sizeof(v->rows[7]),"Automatic start/stop unavailable");
   snprintf(v->rows[8],sizeof(v->rows[8]),"Valid input does not imply in use");
  }
  snprintf(v->footer,sizeof(v->footer),"UP/DOWN Detail  L/R Page");
 }else if(u->page==UI_BMS){
  snprintf(v->title,sizeof(v->title),"Battery monitoring %s",d->bms_online?"online":"offline");
  if(!d->bms_online)snprintf(v->rows[0],sizeof(v->rows[0]),"No valid BMS data");
  else if(u->detail_page==0){
   row_num(v,0,"Pack voltage",d->bms.voltage_10mv,100,"V");row_num(v,1,"Pack current",d->bms.current_10ma,100,"A");row_num(v,2,"SOC",d->bms.soc_percent,0,"%");row_num(v,3,"SOH",d->bms.soh_percent,0,"%");
   uint16_t min=65535,max=0;unsigned cells=0;for(unsigned i=0;i<16;i++)if(d->bms.cell_mv[i]){if(d->bms.cell_mv[i]<min)min=d->bms.cell_mv[i];if(d->bms.cell_mv[i]>max)max=d->bms.cell_mv[i];cells++;}
   if(cells){row_num(v,4,"Min cell",min,0,"mV");row_num(v,5,"Max cell",max,0,"mV");row_num(v,6,"Cell gap",max-min,0,"mV");}
   snprintf(v->rows[7],sizeof(v->rows[7]),"Warning %04X Protect %04X",d->bms.warning,d->bms.protection);
  }else{unsigned start=(u->detail_page-1)*8;for(unsigned r=0;r<8;r++)if(d->bms.cell_mv[start+r])snprintf(v->rows[r],sizeof(v->rows[r]),"Cell %02u: %umV",start+r+1,d->bms.cell_mv[start+r]);else snprintf(v->rows[r],sizeof(v->rows[r]),"Cell %02u: --",start+r+1);}
  snprintf(v->footer,sizeof(v->footer),"UP/DOWN Cells  L/R Page");
 }else if(u->page==UI_ALARM){
  snprintf(v->title,sizeof(v->title),"Alarm information");
  if(d->status_valid){
   snprintf(v->rows[0],sizeof(v->rows[0]),"Fault %lu",(unsigned long)d->status.fault);
   snprintf(v->rows[1],sizeof(v->rows[1]),"Warning %04X DSP BMS %04X",d->status.warning[1],d->status.warning[2]);
  }else{
   snprintf(v->rows[0],sizeof(v->rows[0]),"DSP alarm status unavailable");
   if(d->bms_online)snprintf(v->rows[1],sizeof(v->rows[1]),"BMS warning %04X protect %04X",d->bms.warning,d->bms.protection);
  }
  /* The assets preserve original message indices; warning/fault mapping is explicit. */
  unsigned found=0, shown=0;
  for(unsigned kind=0;kind<3;kind++)for(unsigned bit=0;bit<(kind==0?64U:32U);bit++){
   uint64_t mask=kind==0?(d->status_valid?d->status.warning[0]|((uint64_t)d->status.warning[1]<<16)|((uint64_t)d->status.warning[2]<<32):0):kind==1?(d->status_valid?d->status.fault:0):d->bms_online?(uint64_t)(d->bms.warning|((uint32_t)d->bms.protection<<16)):0;
   if(!(mask&(1ULL<<bit)))continue;
   unsigned table_index=kind==2 && bit>=16?bit+6:bit;
   const char *message=direct_alarm_message(kind==0?DIRECT_ALARM_WARNING:kind==1?DIRECT_ALARM_FAULT:DIRECT_ALARM_BMS,table_index);
   if(found++<u->selected)continue;
   if(shown<6){
    char localized[512];
    const char *text=direct_ui_translate(message?message:"Unmapped alarm bit",d->language_zh,localized,sizeof(localized));
    shown+=alarm_lines(v,2+shown,text,6-shown);
   }
  }
  if(!d->status_valid && !d->bms_online)snprintf(v->rows[2],sizeof(v->rows[2]),"Alarm status unavailable");
  else if(!found)snprintf(v->rows[2],sizeof(v->rows[2]),"No alarm in available sources");
  else if(!shown)snprintf(v->rows[2],sizeof(v->rows[2]),"End of active alarms / UP");
  snprintf(v->footer,sizeof(v->footer),"UP/DOWN Scroll  ESC Back");
 }else if(u->page==UI_PRODUCTION){
  snprintf(v->title,sizeof(v->title),"Manufacturing");
  snprintf(v->rows[0],sizeof(v->rows[0]),"%s",d->factory_ready?"Identity verified":"Awaiting provisioning");
  snprintf(v->rows[1],sizeof(v->rows[1]),"%s",d->model?d->model:"CS-L10-6K2");
  snprintf(v->rows[2],sizeof(v->rows[2]),"SN: %s",d->sn&&d->sn[0]?d->sn:"--");
  snprintf(v->rows[3],sizeof(v->rows[3]),"Hardware: %s",d->hardware?d->hardware:"UNSET");
  snprintf(v->rows[4],sizeof(v->rows[4]),"%s",d->factory_locked?"Identity locked":"Identity unlocked");
  snprintf(v->rows[5],sizeof(v->rows[5]),"%s",d->factory_write_enabled?"USB provisioning enabled":"USB inspection only");
  snprintf(v->rows[6],sizeof(v->rows[6]),"Keys: %02lX / 3F",(unsigned long)d->factory_keys);
  snprintf(v->rows[7],sizeof(v->rows[7]),"Calibration / power test pending");
  snprintf(v->footer,sizeof(v->footer),"ESC Back");
 }else if(u->page==UI_UPGRADE){
  snprintf(v->title,sizeof(v->title),"Firmware upgrade");
  static const char *const states[]={"Ready","Accepted","Downloading","Receiving","Verifying","Installing","Cancelling","Cancelled","Restarting","Succeeded","Failed","Rolled back"};
  /* 阶段名与 App 一致(downloading/transferring/verifying/installing...), 本机屏幕
   * 与手机看到同一格; 旧固件用状态名, 直连升级时只能显示笼统的 "Installing"。 */
  static const struct {const char *stage;const char *label;} stage_labels[]={
   {"accepted","Accepted"},{"downloading","Downloading"},{"verifying","Verifying"},
   {"transferring","Transferring"},{"installing","Installing"},
   {"rebooting","Restarting"},{"succeeded","Succeeded"},{"failed","Failed"}};
  unsigned state=(unsigned)d->ota.state;
  const char *label=NULL;
  for(size_t i=0;i<sizeof(stage_labels)/sizeof(stage_labels[0]);++i)
   if(!strcmp(d->ota.stage,stage_labels[i].stage)){label=stage_labels[i].label;break;}
  if(!label)label=state<sizeof(states)/sizeof(states[0])?states[state]:"Unknown";
  if(state==OTA_STATE_SUCCEEDED && d->ota.target!=OTA_TARGET_ESP)label="Transfer complete";
  snprintf(v->rows[0],sizeof(v->rows[0]),"%s",label);
  snprintf(v->rows[1],sizeof(v->rows[1]),"%s / %s",d->ota.source==OTA_SOURCE_LOCAL?"Bluetooth":"Network",d->ota.target==OTA_TARGET_ESP?"ESP32-C3":d->ota.target==OTA_TARGET_ARM?"ARM":d->ota.target==OTA_TARGET_DSP?"DSP":d->ota.target==OTA_TARGET_BMS?"BMS":"--");
  snprintf(v->rows[2],sizeof(v->rows[2]),"%u%%  %lu / %lu B",d->ota.progress,(unsigned long)d->ota.received,(unsigned long)d->ota.total);
  snprintf(v->rows[3],sizeof(v->rows[3]),"%s",d->ota.version);
  snprintf(v->rows[4],sizeof(v->rows[4]),"%s",state==OTA_STATE_FAILED?upgrade_error(d->ota.error_code):d->ota_busy?"Keep device powered on":"Firmware upgrade");
  if(state==OTA_STATE_FAILED && d->ota.error_code[0] && strcmp(d->ota.error_code,"OK"))
   snprintf(v->rows[5],sizeof(v->rows[5]),"%s",d->ota.error_code);
  snprintf(v->rows[6],sizeof(v->rows[6]),"%s",upgrade_detail(&d->ota));
  bool cancel=d->ota_busy && (state==OTA_STATE_ACCEPTED||state==OTA_STATE_DOWNLOADING||state==OTA_STATE_RECEIVING);
  snprintf(v->footer,sizeof(v->footer),"%s",cancel?"ENTER Cancel  ESC Back":"ESC Back");
 }else if(u->page==UI_HELP){
  snprintf(v->title,sizeof(v->title),"Help / device information");
  snprintf(v->rows[0],sizeof(v->rows[0]),"%s",d->model?d->model:"CS-L10-6K2");snprintf(v->rows[1],sizeof(v->rows[1]),"Hardware: %s",d->hardware?d->hardware:"UNSET");
  snprintf(v->rows[2],sizeof(v->rows[2]),"Firmware: %s",d->version?d->version:"");snprintf(v->rows[3],sizeof(v->rows[3]),"SN: %s",d->sn?d->sn:"--");
  snprintf(v->rows[4],sizeof(v->rows[4]),"DSP: UART 115200 8N1");snprintf(v->rows[5],sizeof(v->rows[5]),"BMS: auto RS485 9600 8N1");
  snprintf(v->rows[6],sizeof(v->rows[6]),"Touch: BS818A CLK2 DAT3");snprintf(v->rows[7],sizeof(v->rows[7]),"LCD: CLK6 DATA7 DC9 RESET10");snprintf(v->rows[8],sizeof(v->rows[8]),"CS: hardware low");
 }else if(u->page==UI_TIME){
  const char *names[]={"Year","Month","Day","Hour","Minute","Second","UTC offset minutes"};snprintf(v->title,sizeof(v->title),"Time set");
  for(unsigned r=0;r<7;r++)snprintf(v->rows[r],sizeof(v->rows[r]),"%s: %d",names[r],u->time_fields[r]);
  snprintf(v->rows[7],sizeof(v->rows[7]),"No battery RTC: sync after reboot");v->selected=u->selected;snprintf(v->footer,sizeof(v->footer),"UP/DN Select  ENTER Edit");
 }else if(u->page==UI_APPEARANCE){
  snprintf(v->title,sizeof(v->title),"Appearance");
  snprintf(v->rows[0],sizeof(v->rows[0]),"Theme: %s",d->theme_light?"Light":"Dark");
  snprintf(v->rows[1],sizeof(v->rows[1]),"Language: %s",d->language_zh?"Chinese":"English");
  snprintf(v->rows[2],sizeof(v->rows[2]),"Save appearance");v->selected=u->selected;
 }else if(u->page==UI_FACTORY){
  snprintf(v->title,sizeof(v->title),"%s",u->group==2?"LCD reset":u->group==1?"Restart controller":"Reset local UI settings");
  snprintf(v->rows[1],sizeof(v->rows[1]),"%s",u->group==2?"Reset LCD only?":u->group==1?"Restart ESP controller?":"Reset buzzer, blank and timezone?");
  snprintf(v->rows[3],sizeof(v->rows[3]),"%s",u->confirm?"YES selected":"NO selected");
  snprintf(v->rows[5],sizeof(v->rows[5]),"DSP power settings retained");snprintf(v->footer,sizeof(v->footer),"L/R Yes/No  ENTER Confirm  ESC Back");
 }
 if(u->message[0])snprintf(v->footer,sizeof(v->footer),"%s",u->message);
}
