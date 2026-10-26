#include "direct_ui_locale.h"
#include "direct_ui.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static bool utf8_valid(const char *s){
 while(*s){unsigned char c=(unsigned char)*s++;unsigned extra=c<128?0:(c&0xe0)==0xc0?1:(c&0xf0)==0xe0?2:(c&0xf8)==0xf0?3:99;
  if(extra==99){
   return false;
  }
  while(extra--){
   if(((unsigned char)*s&0xc0)!=0x80){
    return false;
   }
   s++;
  }
 }return true;
}
int main(void){
 char buffer[512];
 assert(!strcmp(direct_ui_translate("Saved / sync incomplete",true,buffer,sizeof(buffer)),"已保存 同步未完成"));
 assert(!strcmp(direct_ui_translate("Save failed / retry",true,buffer,sizeof(buffer)),"保存失败 请重试"));
 for(unsigned i=0;i<84;i++){
  const char *en=direct_ui_param_name(i),*zh=direct_ui_translate(en,true,buffer,sizeof(buffer));
  assert(strcmp(en,zh) && utf8_valid(zh));
 }
 for(unsigned i=0;i<direct_ui_menu_count();i++){
  const char *en=direct_ui_menu_name(i);assert(strcmp(en,direct_ui_translate(en,true,buffer,sizeof(buffer))));
 }
 for(size_t i=0;i<direct_ui_locale_count();i++){
  const char *zh=direct_ui_locale_zh(i);assert(zh && utf8_valid(zh));
  assert(!strcmp(zh,direct_ui_translate(direct_ui_locale_key(i),true,buffer,sizeof(buffer))));
 }
 assert(!direct_ui_locale_key(direct_ui_locale_count()) && !direct_ui_locale_zh(direct_ui_locale_count()));
 assert(!strcmp(direct_ui_translate("Running parameters 5/5",true,buffer,sizeof(buffer)),"运行参数 5/5"));
 assert(!strcmp(direct_ui_translate("0A AC max charge A",true,buffer,sizeof(buffer)),"0A 市电充电流"));
 assert(!strcmp(direct_ui_translate("07 Output voltage dV: 220.0V",true,buffer,sizeof(buffer)),"07 输出电压: 220.0V"));
 assert(!strcmp(direct_ui_translate("01,Vgrid Low!",true,buffer,sizeof(buffer)),"01,市电欠压"));
 assert(!strcmp(direct_ui_translate("50,BMS warning: cell overvoltage.",true,buffer,sizeof(buffer)),"50,BMS告警: 单体过压"));
 const char *unknown="99,Unspecified manufacturer fault";
 assert(!strcmp(direct_ui_translate(unknown,true,buffer,sizeof(buffer)),unknown));
 const char *sn="SN: L10AC276EEA097C";
 assert(!strcmp(direct_ui_translate(sn,true,buffer,sizeof(buffer)),sn));
 assert(!strcmp(direct_ui_translate("Firmware: 0.1.4-test",true,buffer,sizeof(buffer)),"固件: 0.1.4-test"));
 assert(!strcmp(direct_ui_translate("50.00 Hz",true,buffer,sizeof(buffer)),"50.00 Hz"));
 assert(!strcmp(direct_ui_translate("Warning 0000 Protect 0000",true,buffer,sizeof(buffer)),"告警 0000 保护 0000"));
 assert(direct_ui_translate(sn,false,buffer,sizeof(buffer))==sn);
 assert(direct_ui_translate(sn,true,NULL,0)==sn);
 for(size_t cap=1;cap<32;cap++){
  memset(buffer,0x7f,sizeof(buffer));direct_ui_translate("Output source",true,buffer,cap);
  assert(strlen(buffer)<cap && utf8_valid(buffer) && (unsigned char)buffer[cap]==0x7f);
 }
 assert(!strcmp(direct_ui_translate(NULL,true,buffer,sizeof(buffer)),""));
 { char summary[192];snprintf(summary,sizeof(summary),"PASS UI locale: 84 parameters, %u menus, UTF8 truncation, codes, units, identifiers, fallback",(unsigned)direct_ui_menu_count());puts(summary); }
 return 0;
}
