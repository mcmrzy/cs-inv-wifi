/* Local-panel vocabulary only; no legacy DSP parameter/alarm dictionary. */
#include "direct_ui_locale.h"
#include <string.h>
#include <ctype.h>
typedef struct {const char *en,*zh;} entry_t;
static const entry_t entries[]={
 {"Quick settings","逆变器设置"},
 {"Inverter settings","逆变器设置"},
 {"Auxiliary settings","辅助设置"},
 {"Advanced inverter settings","高级逆变器设置"},
 {"Screen test","屏幕测试"},
 {"Wi-Fi","无线网络"},
 {"Cloud","云服务"},
 {"Connected","已连接"},
 {"Disconnected","未连接"},
 {"Unsupported","不支持"},
 {"Communication","通信采集"},
 {"Restart communication?","重启通信采集?"},
 {"Confirming startup","正在确认启动"},
 {"Checking startup health","正在检查启动状态"},
 {"Transferring","正在传输"},
 {"Unknown","未知"},
 {"Transfer complete","传输完成"},
 {"Upgrade already requested","升级请求已存在"},
 {"Invalid upgrade request","升级请求无效"},
 {"Firmware target mismatch","固件目标不匹配"},
 {"Secure download required","需要安全下载"},
 {"Firmware signature missing","缺少固件签名"},
 {"Firmware signature invalid","固件签名无效"},
 {"Security check unavailable","安全检查不可用"},
 {"Older firmware blocked","禁止版本回退"},
 {"Another upgrade is running","另一个升级正在进行"},
 {"Firmware file too large","固件文件过大"},
 {"Firmware size mismatch","固件长度不匹配"},
 {"Firmware checksum mismatch","固件校验不匹配"},
 {"Firmware download failed","固件下载失败"},
 {"Upgrade timed out","升级超时"},
 {"Firmware download interrupted","固件下载中断"},
 {"Firmware write failed","固件写入失败"},
 {"Target transfer failed","目标传输失败"},
 {"Upgrade settings save failed","升级信息保存失败"},
 {"Upgrade cannot be cancelled","当前阶段无法取消"},
 {"Upgrade could not start","无法启动升级"},
 {"Upgrade internal error","升级内部错误"},
 {"Upgrade target unsupported","不支持此升级目标"},
 {"Insufficient firmware storage","固件存储空间不足"},
 {"DSP transfer failed","DSP传输失败"},
 {"BMS transfer failed","BMS传输失败"},
 {"Firmware format invalid","固件格式无效"},
 {"Target not ready","目标尚未就绪"},
 {"Upgrade failed","升级失败"},
 {"Target did not restart","目标运行未恢复"},
 {"Upgrade unconfirmed","升级未确认"},
 {"Diagnostic finished","检查完成"},
 {"Packet 0 only; Boot held","引导程序等待升级"},
 {"Firmware readback unavailable","固件读回未确认"},
 {"Check error code and retry","请按错误码排查后重试"},
 {"Target response received","已收到目标运行应答"},
 {"Target response not observed","尚未收到目标运行应答"},
 {"Target response not confirmed","目标运行应答未确认"},
 {"Waiting for target Boot","等待目标进入引导程序"},
 {"Waiting for target response","等待目标运行应答"},
 {"Send firmware from App or cloud","请通过手机或云端发送固件"},
 {"Preparing firmware upgrade","正在准备固件升级"},
 {"Downloading firmware","正在下载固件"},
 {"Receiving firmware","正在接收固件"},
 {"Checking firmware image","正在校验固件"},
 {"Writing firmware image","正在写入固件"},
 {"Cancelling firmware upgrade","正在取消固件升级"},
 {"Firmware upgrade cancelled","固件升级已取消"},
 {"Restarting controller","正在重启控制器"},
 {"Controller update confirmed","控制器升级已确认"},
 {"Firmware upgrade failed","固件升级失败"},
 {"Previous firmware restored","已恢复旧版固件"},
 {"Upgrade status unavailable","无法获取升级状态"},
 {"Keep device powered on","请保持设备供电"},
 {"Phone connection","手机连接"},
 {"Back","返回"},
 {"Home","主页"},
 {"Prev","上页"},
 {"Next","下页"},
 {"Modes","模式"},
 {"Grid","市电"},
 {"Up","向上"},
 {"Down","向下"},
 {"Left","向左"},
 {"Right","向右"},
 {"Overview","概览"},
 {"Details","详情"},
 {"Summary","摘要"},
 {"Menu","菜单"},
 {"Open","打开"},
 {"Edit","编辑"},
 {"Review","复核"},
 {"Confirm","确认"},
 {"Cancel","取消"},
 {"Change","切换"},
 {"Saving user settings","正在保存设置"},
 {"Hardware","硬件"},
 {"Keys","按键"},
 {"Manufacturing","生产维护"},
 {"Identity verified","出厂信息有效"},
 {"Awaiting provisioning","等待写入出厂信息"},
 {"Identity locked","身份已锁定"},
 {"Identity unlocked","身份未锁定"},
 {"USB provisioning enabled","USB生产写入已开启"},
 {"USB inspection only","USB仅可查询"},
 {"Keys:","按键:"},
 {"Firmware upgrade","固件升级"},
 {"Upgrade busy","正在升级"},
 {"Ready","等待升级"},
 {"Accepted","已接受"},
 {"Downloading","正在下载"},
 {"Receiving","正在接收"},
 {"Verifying","正在校验"},
 {"Installing","正在写入"},
 {"Cancelling","正在取消"},
 {"Cancelled","已取消"},
 {"Restarting","正在重启"},
 {"Succeeded","升级成功"},
 {"Failed","升级失败"},
 {"Rolled back","已回退"},
 {"Bluetooth","蓝牙"},
 {"Weather","天气"},
 {"Network","网络"},
 {"ENTER Cancel  ESC Back","确认取消 返回退出"},
 {"ENTER Cancel","确认 取消升级"},
 {"ENTER Confirm  ESC Back","确认 停止升级 返回继续"},
 {"Stop this firmware transfer?","停止本次固件传输?"},
 {"Hardware:","硬件:"},
 {"System","面板设置"},
 {"Wi-Fi setup","Wi-Fi 配网"},
 {"Panel BMS protocol","面板电池协议"},
 {"Link","协议"},
 {"ENTER Edit","确认 编辑"},
 {"LCD reset","屏幕复位"},
 {"Invalid calendar date","日期无效"},
 {"Energy flow","能流概览"},
 {"Data","详情"},
 {"Battery","电池"},
 {"Help","帮助"},
 {"Time set","时间设置"},
 {"Restart","重启"},
 {"System menu","系统菜单"},
 {"Running parameters","运行参数"},
 {"Theme","主题"},
 {"Language","语言"},
 {"Save","保存"},
 {"Save appearance","保存显示设置"},
 {"Appearance saved","显示设置已保存"},
 {"Save failed / retry","保存失败请重试"},
 {"Appearance","显示设置"},
 {"Dark","深色"},
 {"Light","浅色"},
 {"English","英文"},
 {"Chinese","中文"},
 {"GRID","市电"},
 {"PV","光伏"},
 {"BAT","电池"},
 {"LOAD","负载"},
 {"valid","有效"},
 {"Load","负载"},
 {"Inverter","逆变"},
 {"Data stale","数据过期"},
 {"Fault","故障"},
 {"Controller","控制器"},
 {"Display","屏幕"},
 {"Firmware","固件"},
 {"Touch","按键"},
 {"LCD","屏幕"},
 {"Year","年"},
 {"Month","月"},
 {"Day","日"},
 {"Hour","时"},
 {"Minute","分"},
 {"Second","秒"},
 {"Time zone","时区"},
 {"Restart controller","重启控制器"},
 {"Reset local UI settings","恢复本机界面设置"},
 {"YES","是"},
 {"NO","否"},
 {"YES selected","已选 是"},
 {"NO selected","已选 否"},
 {"Save rejected","保存未接受"},
 {"ESC Back","退出 返回"},
 {"ESC Exit","退出"},
 {"ENTER Confirm","确认 保存"},
 {"UP/DN Select","上下 选择"},
 {"L/R Edit","左右 修改"},
 {"L/R Choose","左右 选择"},
 {"LCD contrast test","屏幕对比测试"},
 {"Gray ramp: black to white","黑到白灰阶"},
 {"Black text on white","白底黑字"},
 {"Compare same angle in A and B","同角度比较A和B"},
 {"L/R Info, A, B","左右 信息 A B"},
 {"B: INV ON","B: 反色开"},
 {"A: INV OFF","A: 反色关"},
 {"ENTER Open","确认 打开"},
 {"ESC Cancel","退出 取消"},
 {"Device information","设备信息"},
 {"Information","信息"},
 {"Time not synced","时间未同步"},
 {"Time saved","时间已保存"},
 {"Time zone saved","时区已保存"},
 {"Time zone changes display only","时区仅改变显示时间"},
 {"Saved / sync incomplete","已保存 同步未完成"},
 {"Invalid date / save failed","日期无效或保存失败"},
 {"L/R Yes/No  ENTER Confirm  ESC Back","左右是否 确认保存 退出返回"},
 {"Inverter settings","参数设置"},
 {"Power control","开关机"},
 {"Faults / warnings","故障告警"},
 {"Service","维护诊断"},
 {"Communication settings","逆变器通信设置"},
 {"Communication diagnostics","逆变器通信诊断"},
 {"Local settings saved","面板设置已保存"},
 {"Local settings restored","面板设置已恢复"},
 {"Saving local settings","正在保存面板设置"},
 {"Select YES to continue","选择是后再确认"},
 {"Value limit reached","已到允许边界"},
 {"Sync time after restart","重启后请联网同步时间"},
 {"Inverter settings retained","保留逆变器参数"},
 {"Reset display, sound and time?","恢复显示声音和时间设置?"},
 {"Live data","实时数据"},
 {"No live data","数据未就绪"},
 {"Use USB service for provisioning","通过USB服务写入出厂信息"},
 {"Open inverter functions from menu","请从菜单打开逆变器功能"},
 {"Alarms","告警"},
 {"Alarm","告警"},
 {"BMS","BMS"},
 {"Choose","选择"},
 {"Backlight","背光亮度"},
 {"Screen timeout","自动息屏"},
 {"Never","永不"},
 {"Modbus","Modbus"},
 {"SN","SN"},
 {"UNSET","未设置"},
 {"Reset LCD only?","仅复位显示屏?"},
 {"Reset display?","复位显示屏?"},
 {"Screen restored","屏幕已恢复"},
 {"Screen reset failed","屏幕复位失败"},
 {"Settings busy / retry","正在保存请稍后"},
 {"Result unconfirmed","结果未确认"},
 {"Accepted by inverter","协议设置成功"},
};
size_t direct_ui_locale_count(void){return sizeof(entries)/sizeof(entries[0]);}
const char *direct_ui_locale_key(size_t i){return i<direct_ui_locale_count()?entries[i].en:NULL;}
const char *direct_ui_locale_zh(size_t i){return i<direct_ui_locale_count()?entries[i].zh:NULL;}
static const char *lookup(const char *s,size_t len){
 for(size_t i=0;i<direct_ui_locale_count();i++)if(strlen(entries[i].en)==len && !memcmp(s,entries[i].en,len))return entries[i].zh;
 return NULL;
}
/* Safe append: copy whole UTF8 sequences, never a partial Chinese glyph. */
static void append(char *out,size_t cap,size_t *used,const char *s,size_t len){
 size_t i=0;while(i<len){unsigned char c=(unsigned char)s[i];size_t n=c<128?1:(c&0xe0)==0xc0?2:(c&0xf0)==0xe0?3:(c&0xf8)==0xf0?4:1;
  if(i+n>len || *used+n>=cap)break;
  memcpy(out+*used,s+i,n);*used+=n;i+=n;
 }out[*used]=0;
}
static void translate_part(const char *s,size_t len,char *out,size_t cap,size_t *used,unsigned depth){
 if(depth>4){append(out,cap,used,s,len);return;}
 const char *zh=lookup(s,len);if(zh){append(out,cap,used,zh,strlen(zh));return;}
 /* Preserve original fault code or numbered menu prefix. */
 size_t prefix=0;while(prefix<len && isdigit((unsigned char)s[prefix]))prefix++;
 if(len>3 && isxdigit((unsigned char)s[0]) && isxdigit((unsigned char)s[1]) && s[2]==' ')
  prefix=2;
 if(prefix && prefix<len && (s[prefix]==',' || s[prefix]==' ')){
  prefix++;append(out,cap,used,s,prefix);translate_part(s+prefix,len-prefix,out,cap,used,depth+1);return;
 }
 const char *colon=memchr(s,':',len);
 if(colon){size_t left=(size_t)(colon-s);translate_part(s,left,out,cap,used,depth+1);
  append(out,cap,used,":",1);size_t next=left+1;while(next<len && s[next]==' '){append(out,cap,used," ",1);next++;}
  translate_part(s+next,len-next,out,cap,used,depth+1);return;
 }
 /* Translate source/target pairs independently. */
 for(size_t i=0;i+3<=len;i++)if(!memcmp(s+i," / ",3)){
  translate_part(s,i,out,cap,used,depth+1);append(out,cap,used," / ",3);
  translate_part(s+i+3,len-i-3,out,cap,used,depth+1);return;
 }
 for(size_t i=0;i+3<=len;i++)if(!memcmp(s+i," > ",3)){
  translate_part(s,i,out,cap,used,depth+1);append(out,cap,used," > ",3);
  translate_part(s+i+3,len-i-3,out,cap,used,depth+1);return;
 }
 /* Dynamic numeric title/status suffix. Exact prefix must end at a word boundary. */
 for(size_t i=0;i<direct_ui_locale_count();i++){
  size_t n=strlen(entries[i].en);
  if(n<len && s[n]==' ' && !memcmp(s,entries[i].en,n) &&
     (isdigit((unsigned char)s[n+1]) || s[n+1]=='-' || s[n+1]=='%' || s[n+1]=='/')){
   append(out,cap,used,entries[i].zh,strlen(entries[i].zh));
   append(out,cap,used," ",1);
   translate_part(s+n+1,len-n-1,out,cap,used,depth+1);return;
  }
 }
 append(out,cap,used,s,len);
}
const char *direct_ui_translate(const char *s,bool zh,char *out,size_t cap){
 if(!s)s="";
 if(!zh || !out || !cap)return s;
 size_t used=0;out[0]=0;translate_part(s,strlen(s),out,cap,&used,0);return out;
}
