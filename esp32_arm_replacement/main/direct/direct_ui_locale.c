#include "direct_ui_locale.h"
#include <string.h>
#include <ctype.h>
typedef struct { const char *en,*zh; } entry_t;
#define E(a,b) {a,b}
static const entry_t entries[]={
 E("Transferring","正在传输"),E("Unknown","状态未知"),E("Transfer complete","传输完成"),
 E("Upgrade already requested","升级请求已存在"),E("Invalid upgrade request","升级请求无效"),
 E("Firmware target mismatch","固件目标不匹配"),E("Secure download required","需要安全下载"),
 E("Firmware signature missing","缺少固件签名"),E("Firmware signature invalid","固件签名无效"),
 E("Security check unavailable","安全检查不可用"),E("Older firmware blocked","禁止版本回退"),
 E("Another upgrade is running","另一个升级正在进行"),E("Firmware file too large","固件文件过大"),
 E("Firmware size mismatch","固件长度不匹配"),E("Firmware checksum mismatch","固件校验不匹配"),
 E("Firmware download failed","固件下载失败"),E("Upgrade timed out","升级超时"),
 E("Firmware download interrupted","固件下载中断"),E("Firmware write failed","固件写入失败"),
 E("Target transfer failed","目标传输失败"),E("Upgrade settings save failed","升级信息保存失败"),
 E("Upgrade cannot be cancelled","当前阶段无法取消"),E("Upgrade could not start","无法启动升级"),
 E("Upgrade internal error","升级内部错误"),E("Upgrade target unsupported","不支持此升级目标"),
 E("Insufficient firmware storage","固件存储空间不足"),E("DSP transfer failed","DSP传输失败"),
 E("BMS transfer failed","BMS传输失败"),E("Firmware format invalid","固件格式无效"),
 E("Target not ready","目标尚未就绪"),E("Upgrade failed","升级失败"),
 E("Target did not restart","目标运行未恢复"),E("Upgrade unconfirmed","升级未确认"),
 E("Diagnostic finished","检查完成"),E("Packet 0 only; Boot held","引导程序等待升级"),
 E("Firmware readback unavailable","固件读回未确认"),
 E("Check error code and retry","请按错误码排查后重试"),E("Target response received","已收到目标运行应答"),
 E("Target response not observed","尚未收到目标运行应答"),E("Target response not confirmed","目标运行应答未确认"),
 E("Waiting for target Boot","等待目标进入引导程序"),E("Waiting for target response","等待目标运行应答"),
 E("Send firmware from App or cloud","请通过手机或云端发送固件"),E("Preparing firmware upgrade","正在准备固件升级"),
 E("Downloading firmware","正在下载固件"),E("Receiving firmware","正在接收固件"),
 E("Checking firmware image","正在校验固件"),E("Writing firmware image","正在写入固件"),
 E("Cancelling firmware upgrade","正在取消固件升级"),E("Firmware upgrade cancelled","固件升级已取消"),
 E("Restarting controller","正在重启控制器"),E("Controller update confirmed","控制器升级已确认"),
 E("Firmware upgrade failed","固件升级失败"),E("Previous firmware restored","已恢复先前固件"),
 E("Upgrade status unavailable","升级状态未知"),E("Keep device powered on","请保持设备供电"),
 E("Phone connection","手机连接"),E("Scan to connect","扫码连接"),
 E("Network online","网络在线"),E("Network offline","网络离线"),
 E("Back","返回"),E("Home","主页"),E("Prev","上页"),E("Next","下页"),
 E("Up","向上"),E("Down","向下"),E("Left","向左"),E("Right","向右"),
 E("Overview","概览"),E("Details","详情"),E("Summary","摘要"),E("Cells","单体"),
 E("Menu","菜单"),E("Open","打开"),E("Edit","编辑"),E("Review","复核"),
 E("Confirm","确认"),E("Cancel","取消"),E("Change","切换"),E("Fast+","快调+"),E("Fast-","快调-"),
 E("Saving user settings","保存用户设置"),E("Sending settings","发送设置"),
 E("Hardware","硬件"),E("Keys","按键"),
 E("Manufacturing","生产维护"),E("Identity verified","身份已验证"),E("Awaiting provisioning","等待写入身份"),E("Identity locked","身份已锁定"),E("Identity unlocked","身份未锁定"),E("USB provisioning enabled","USB写入已启用"),E("USB inspection only","USB只读查询"),E("Calibration / power test pending","校准与带载测试待验证"),E("Keys:","按键:"),
 E("Firmware upgrade","固件升级"),E("Upgrade busy","正在升级"),E("Ready","等待升级"),E("Accepted","已接受"),E("Downloading","正在下载"),E("Receiving","正在接收"),E("Verifying","正在校验"),E("Installing","正在写入"),E("Cancelling","正在取消"),E("Cancelled","已取消"),E("Restarting","正在重启"),E("Succeeded","升级成功"),E("Failed","升级失败"),E("Rolled back","已回退"),E("Bluetooth","蓝牙"),E("Network","网络"),E("DSP / BMS direct upgrade","DSP/BMS升级"),E("Use App or cloud to send firmware","通过手机或云端发送固件"),E("ENTER Cancel  ESC Back","确认取消 返回退出"),E("Hardware:","硬件:"),
 E("Input / Output","输入与输出"),E("Battery / EQ","电池与均衡"),E("System","系统"),E("Advanced","高级参数"),
 E("Current value","当前值"),E("New value","拟修改值"),E("Review change","复核修改"),
 E("L/R step  UP/DN fast","左右微调 上下快调"),E("ENTER Review","确认 复核"),E("ENTER Edit","确认 编辑"),
 E("LCD reset","屏幕复位"),E("Reset display?","复位屏幕？"),
 E("Generator information","发电机信息"),E("Current:","当前值:"),E("Draft:","拟修改值:"),
 E("Invalid calendar date","日期无效"),E("L/R Page ENTER Set","左右切页 确认设置"),
 E("UP/DOWN Detail L/R Page","上下详情 左右切页"),E("UP/DOWN Cells L/R Page","上下单体 左右切页"),
 E("UP/DN Group","上下 分组"),E("ENTER Cells","确认 单体"),
 E("Energy overview","能流概览"),
 E("Output source","输出优先"),E("Max charge A","总充电流"),E("AC input range","输入范围"),
 E("Battery capacity Ah","电池容量"),E("Battery type","电池类型"),E("Overload restart","过载重启"),
 E("Overtemp restart","过温重启"),E("Output voltage dV","输出电压"),E("Output frequency","输出频率"),
 E("Master/slave","主从设置"),E("AC max charge A","市电充电流"),E("Back to utility dV","回市电压"),
 E("Back to battery dV","回电池压"),E("Charge source","充电优先"),E("Alarm control","告警开关"),
 E("Screen blank","屏幕休眠"),E("Source lost alarm","断电告警"),E("Overload bypass","过载旁路"),
 E("Parallel discharge A","并机放电流"),E("Parallel charge A","并机充电流"),E("Cutoff recover dV","电压恢复差"),
 E("Solar power balance","光伏平衡"),E("Output mode","输出模式"),E("Lithium material","锂电材料"),
 E("LiFePO4 cells","铁锂串数"),E("Li-NMC cells","三元串数"),E("Equalization enable","均衡开关"),
 E("Equalization dV","均衡电压"),E("Equalization min","均衡时长"),E("Eq timeout min","均衡超时"),
 E("Eq interval days","均衡间隔"),E("Eq activate","均衡激活"),E("AC charge start hour","充电开始时"),
 E("AC charge end hour","充电结束时"),E("Generator start dV","发电启动压"),E("Generator stop dV","发电停止压"),
 E("Utility start SOC","回市电量"),E("Battery return SOC","回电池量"),E("Generator start SOC","发电启动量"),
 E("Generator stop SOC","发电停止量"),E("Cutoff SOC","截止电量"),E("Buzzer","蜂鸣器"),
 E("Socket S SOC","插座S电量"),E("Socket A SOC","插座A电量"),E("Socket B SOC","插座B电量"),
 E("Manual socket","手动插座"),E("Control socket","插座控制"),E("SOC recover gap","电量恢复差"),
 E("BMS protocol","BMS协议"),E("Generator power W","发电额定功率"),E("Generator balance","发电平衡"),
 E("Gen start1 low","发电始1低字"),E("Gen start1 high","发电始1高字"),
 E("Gen end1 low","发电终1低字"),E("Gen end1 high","发电终1高字"),
 E("Gen start2 low","发电始2低字"),E("Gen start2 high","发电始2高字"),
 E("Gen end2 low","发电终2低字"),E("Gen end2 high","发电终2高字"),
 E("Output start1 low","输出始1低字"),E("Output start1 high","输出始1高字"),
 E("Output end1 low","输出终1低字"),E("Output end1 high","输出终1高字"),
 E("Output start2 low","输出始2低字"),E("Output start2 high","输出始2高字"),
 E("Output end2 low","输出终2低字"),E("Output end2 high","输出终2高字"),
 E("AGM bulk dV","AGM恒压"),E("Flooded bulk dV","富液恒压"),E("User Pb bulk dV","铅酸恒压"),
 E("AGM float dV","AGM浮充"),E("Flooded float dV","富液浮充"),E("Lithium float dV","锂电浮充"),
 E("User Pb float dV","铅酸浮充"),E("User LFP float dV","铁锂浮充"),E("User NMC float dV","三元浮充"),
 E("AGM cutoff dV","AGM截止"),E("Flooded cutoff dV","富液截止"),E("Lithium cutoff dV","锂电截止"),
 E("User Pb cutoff dV","铅酸截止"),E("User LFP cutoff dV","铁锂截止"),E("User NMC cutoff dV","三元截止"),
 E("AC charge max SOC","市电充电量"),E("AC charge max dV","市电充电压"),
 E("Input","输入"),E("Output","输出"),E("Charging","充电"),E("Battery","电池"),
 E("Generator","发电机"),E("Other","其他"),E("Help","帮助"),E("Equalization","均衡"),
 E("Battery monitoring","电池监控"),E("Parameter information","运行参数"),E("Alarm information","告警信息"),
 E("Time set","时间设置"),E("All parameters","全部参数"),E("Restart","重启"),E("Factory reset","恢复设置"),
 E("System menu","系统菜单"),E("Input set","输入设置"),E("Output set","输出设置"),
 E("Charging set","充电设置"),E("Battery set","电池设置"),E("Generator set","发电机设置"),
 E("Other set","其他设置"),E("Running parameters","运行参数"),E("Theme","主题"),E("Language","语言"),
 E("Apply","保存"),E("Save","保存"),E("Save appearance","保存显示设置"),E("Appearance saved","显示设置已保存"),
 E("Save failed / retry","保存失败 请重试"),E("L/R Change","左右 切换"),E("Appearance","显示设置"),E("Dark","深色"),E("Light","浅色"),
 E("English","英文"),E("Chinese","中文"),E("Dark theme","深色主题"),E("Light theme","浅色主题"),
 E("On","开启"),E("Off","关闭"),E("Solar first","光伏优先"),E("Utility first","市电优先"),
 E("Solar+Utility","光伏加市电"),E("Only solar","仅光伏"),E("S>B>U","光伏电池市电"),E("S>U>B","光伏市电电池"),
 E("Appliance","家电"),E("UPS","UPS"),E("AGM","AGM"),E("Flooded","富液"),
 E("User Pb","自定铅酸"),E("Lithium","锂电"),E("User Li","自定锂电"),
 E("GRID","市电"),E("PV","光伏"),E("BAT","电池"),E("LOAD","负载"),
 E("ONLINE","在线"),E("OFFLINE","离线"),E("Online","在线"),E("Offline","离线"),
 E("online","在线"),E("offline","离线"),E("DSP online","DSP在线"),E("DSP offline","DSP离线"),
 E("Valid","有效"),E("Not valid","无效"),E("valid","有效"),E("not valid","无效"),
 E("AC input","市电输入"),E("AC frequency","市电频率"),E("PV power","光伏功率"),
 E("Battery SOC","电池电量"),E("Load","负载"),E("Temperature","温度"),E("Grid current","市电电流"),
 E("PV1 voltage","光伏1电压"),E("PV1 current","光伏1电流"),E("PV2 voltage","光伏2电压"),E("PV2 current","光伏2电流"),
 E("Bus","母线"),E("Output current","输出电流"),E("Load ratio","负载比例"),E("Inverter","逆变"),
 E("Inverter current","逆变电流"),E("Inverter freq","逆变频率"),E("Inverter power","逆变功率"),
 E("Inverter apparent","逆变视在"),E("Battery charge","电池充电"),E("Battery discharge","电池放电"),
 E("Load apparent","负载视在"),E("Grid power","市电功率"),E("Bus current","母线电流"),
 E("PV1 power","光伏1功率"),E("PV2 power","光伏2功率"),E("AC in","市电输入"),E("AC out","市电输出"),
 E("Today / Total kWh (ESP meter)","今日/累计kWh"),E("Today / Total kWh","今日/累计kWh"),
 E("Since ESP install / NVS checkpoint","从ESP安装起计量"),E("Generator: -- (no power channel)","发电机: -- 未计量"),
 E("Generator monitoring only","发电机监控"),E("Generator input","发电输入"),E("Utility input","市电输入"),
 E("Generator power","发电功率"),E("Generator energy","发电电量"),E("No independent metering","无独立计量"),
 E("No independent power channel","无独立功率数据"),E("Automatic start/stop unavailable","不支持自动启停"),
 E("Valid input does not imply in use","输入有效不代表使用"),E("Source status unavailable","输入状态未知"),
 E("Pack voltage","电池总压"),E("Pack current","电池电流"),E("SOC","电量"),E("SOH","健康度"),
 E("Min cell","最低单体"),E("Max cell","最高单体"),E("Cell gap","单体压差"),E("Cell","单体"),
 E("No valid BMS data","暂无BMS数据"),E("No cell measurements","暂无单体数据"),E("BMS not responding","BMS未响应"),
 E("Waiting for BMS","等待BMS"),E("Data stale","数据过期"),E("Live data unavailable","实时数据未知"),E("Waiting for live status","等待实时状态"),
 E("Alarm status unavailable","告警状态未知"),E("DSP alarm status unavailable","DSP告警状态未知"),
 E("Communication unavailable","通信不可用"),E("No alarm in available sources","有效来源暂无告警"),
 E("End of active alarms / UP","告警末尾 上键返回"),E("Unmapped alarm bit","未定义告警位"),
 E("Fault","故障"),E("Warning","告警"),E("Protect","保护"),E("Mode","模式"),E("InvS","逆变状态"),
 E("Polls","轮询"),E("CRC errors","校验错"),E("BMS warning","BMS告警"),E("BMS protection","BMS保护"),
 E("Help / device information","帮助和设备信息"),E("Controller","控制器"),E("Display","屏幕"),
 E("Firmware","固件"),E("Touch","触摸按键"),E("LCD","屏幕"),E("CS: hardware low","CS: 硬件恒低"),
 E("No battery RTC: sync after reboot","重启后请同步时间"),E("Year","年"),E("Month","月"),E("Day","日"),
 E("Hour","时"),E("Minute","分"),E("Second","秒"),E("UTC offset minutes","时区偏移分钟"),
 E("Restart controller","重启控制器"),E("Reset local UI settings","恢复本机界面设置"),
 E("Reset UI preferences","恢复界面设置"),E("Restart ESP controller?","重启ESP控制器?"),
 E("Restart ESP32-C3?","重启ESP32-C3?"),E("Reset local preferences?","恢复本机界面设置?"),
 E("Reset buzzer, blank and timezone?","恢复蜂鸣休眠和时区?"),E("DSP power settings retained","保留DSP功率设置"),
 E("YES","是"),E("NO","否"),E("YES selected","已选 是"),E("NO selected","已选 否"),
 E("Saved / confirmed","已保存并确认"),E("No confirmation / timeout","未确认 已超时"),E("Save rejected","保存被拒绝"),
 E("Unknown parameter","参数未知"),E("Read only / unavailable","只读或不可用"),E("DSP offline / write blocked","DSP离线 禁止写入"),
 E("Value out of range","数值超出范围"),E("Waiting for DSP confirmation","等待DSP确认"),
 E("ENTER Set","确认 设置"),E("ESC Back","退出 返回"),E("ESC Exit","退出"),
 E("ENTER Confirm","确认 保存"),E("ENTER Save","确认 保存"),E("UP/DN Select","上下 选择"),
 E("UP/DN Scroll","上下 滚动"),E("UP/DOWN Scroll","上下 滚动"),E("L/R Edit","左右 修改"),
 E("L/R Page","左右 翻页"),E("LEFT/RIGHT Page","左右 翻页"),E("L/R Choose","左右 选择"),
 E("RIGHT Cells","右键 单体"),E("L/R Summary","左右 摘要"),E("L/R LCD Test","左右 屏幕测试"),
 E("LCD contrast test","屏幕对比测试"),E("Gray ramp: black to white","黑到白灰阶"),
 E("Black text on white","白底黑字"),E("Compare same angle in A and B","同角度比较A和B"),
 E("L/R Info, A, B","左右 信息 A B"),E("B: INV ON","B: 反色开"),E("A: INV OFF","A: 反色关"),
 E("Vgrid Low!","市电欠压"),E("Vgen Low!","发电欠压"),E("Vinvt Low!","逆变欠压"),E("Vload Low!.","输出欠压"),
 E("Vbus Low!","母线欠压"),E("Vgrid High!","市电过压"),E("Vgen High!","发电过压"),E("Vload High!","输出过压"),
 E("Vinvt High!","逆变过压"),E("Vpv1 High!","光伏1过压"),E("Vpv2 High!","光伏2过压"),E("Vbus High!","母线过压"),
 E("Iload High!","输出过流"),E("Iinvt High!","逆变过流"),E("Vpv1 Low!","光伏1欠压"),E("Vpv2 Low","光伏2欠压"),
 E("Vbat Low!","电池欠压"),E("Vbat High!","电池过压"),E("Fan1 Speed Low!","风扇1转速低"),E("Fan2 Speed Low!","风扇2转速低"),
 E("Dc Component High!","直流分量过高"),E("Pv1OverCurrent!","光伏1过流"),E("Pv2OverCurrent!","光伏2过流"),E("HwOverCurrent!","硬件过流"),
 E("BMSCommLoss!","BMS通信丢失"),E("cellOverVoltage!","单体过压"),E("cellUnderVoltage!","单体欠压"),
 E("Bat Over Charged!","电池过充"),E("Bat Over DisCharged!","电池过放"),
 E("Bat Discharge Over Temperature!","电池放电过温"),E("Bat Charge Over Temperature!","电池充电过温"),
 E("BMS MOS Over Temperature!","BMS管过温"),E("Bat Over Temperature!","电池过温"),E("Bat Under Temperature!","电池低温"),
 E("System Shut Down!","系统关闭"),E("BMS Comm Error!","BMS通信错误"),E("Bat Disconnect!","电池断开"),E("Out Power Derating!","输出降额"),
 E("Vbus Short Circuit!","母线短路"),E("Arm Com Error!","控制通信错误"),E("Pre-Charge Fail!","预充失败"),
 E("Fan1 Locked!","风扇1堵转"),E("Fan2 Locked!","风扇2堵转"),E("Ibus High!","母线过流"),
 E("RelayVinvtSticking!","逆变继电器粘连"),E("RelayAcSticking!","交流继电器粘连"),
 E("RelayVinvtOpenCircuit!","逆变继电器开路"),E("RelayAcOpenCircuit!","交流继电器开路"),
 E("VinvtOut!","逆变输出异常"),
 E("AC INPUT","市电输入"),E("Status unknown","状态未知"),E("Fault active","存在故障"),
 E("Warning active","存在告警"),E("Normal","正常"),E("ENTER Menu","确认 菜单"),
 E("ENTER Open","确认 打开"),E("Settings","设置"),E("Waiting for confirmation","等待确认"),
 E("LEFT - / RIGHT +","左减 右加"),E("Sending to inverter","发送到逆变器"),E("Unsaved change","尚未保存"),
 E("ESC Cancel","退出 取消"),E("READ ONLY","只读"),E("Device information","设备信息"),
 E("Time settings","时间设置"),E("Active alarms","当前告警"),E("Information","信息"),
 E("Input status unknown","输入状态未知"),E("Generator input valid","发电输入有效"),
 E("Generator input not valid","发电输入无效"),E("Direct DSP / no ARM UART","直连DSP 无ARM中转"),
 E("BMS fault","BMS故障"),E("cell overvoltage.","单体过压"),E("cell undervoltage.","单体欠压"),
 E("overvoltage.","电池过压"),E("undervoltage.","电池欠压"),E("charge over current.","充电过流"),
 E("discharge over current.","放电过流"),E("charge over temperature.","充电过温"),
 E("discharge over temperature.","放电过温"),E("charge under temperature.","充电低温"),
 E("discharge under temperature.","放电低温"),E("environment over temperature.","环境过温"),
 E("environment under temperature.","环境低温"),E("MOS over temperature.","MOS过温"),E("low SOC.","电量低"),
 E("discharge mosfet fault.","放电管故障"),E("temperature sensor fault.","温度传感故障"),
 E("invalid cell fault.","单体异常"),E("front end sampling communication fault ","采样通信故障"),
 E("charger inversed.","充电器反接"),E("cell overvoltage protection.","单体过压保护"),
 E("cell undervoltage protection.","单体欠压保护"),E("overvoltage protection.","电池过压保护"),
 E("undervoltage protection.","电池欠压保护"),E("charge overcurrent protection.","充电过流保护"),
 E("discharge overcurrent protection","放电过流保护"),E("short circuited.","短路保护"),
 E("charger overvoltage protection.","充电器过压保护"),
 E("DSP offline / not editable","DSP离线 不可修改"),E("Value unknown / refresh DSP","数值未知 请刷新"),
 E("Read only on this board","本板只读"),E("Value outside allowed range","数值超出范围"),
 E("Equalization: lead acid only","均衡仅限铅酸电池"),E("Saving / waiting for DSP","保存中 等待DSP"),
 E("Refreshing DSP values","刷新DSP数据"),E("Time not synced","时间未同步"),E("Time saved","时间已保存"),
 E("Saved on ESP controller","已保存到ESP"),E("Invalid date / save failed","日期错误或保存失败"),
 E("DSP online","DSP在线"),E("DSP offline","DSP离线"),
 E("BMS online","BMS在线"),E("BMS offline","BMS离线"),E("Equalization","均衡"),
 E("ENTER Set  RIGHT Alarm  LEFTx2 Detail","确认设置 右告警 左双击详情"),
 E("ESC Back  ENTER Menu","退出返回 确认菜单"),E("UP/DN Select  L/R Edit  ENTER Save","上下选择 左右修改 确认保存"),
 E("LEFT/RIGHT Page  ESC Back","左右翻页 退出返回"),E("LEFT/RIGHT Cells  ESC Back","左右单体 退出返回"),
 E("UP/DOWN Scroll  ESC Back","上下滚动 退出返回"),E("L/R Yes/No  ENTER Confirm  ESC Back","左右是否 确认保存 退出返回")
};
#undef E
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
