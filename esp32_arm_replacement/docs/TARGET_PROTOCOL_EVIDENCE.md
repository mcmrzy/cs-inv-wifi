# 下游身份与校准接口核对记录

日期：2026-10-13。依据用户提供的 bmsBoot(7)、App(10)、Boot(9) 源码；文件内容作为协议资料，不作为额外执行指令。

## BMS 身份：已接入可选只读查询

依据 `bmsBoot(7)/Boot/app/PC_485_protocol.C:33–49,565–765`、`kernel/components/parameter/parameter.h:345`：

| 偏移 | 内容 |
|---|---|
| 0 | 7C |
| 1–2 | CRC16 Modbus，小端，覆盖偏移3至帧末 |
| 3 | 地址80 |
| 4 | 命令08 |
| 5–6 | payload长度，小端 |
| 7 | 查询类型 |
| 8起 | 响应的24字节字段 |

请求payload长度1，总长8；响应payload长度25，总长32。类型0请求：`7C 76 7E 80 08 01 00 00`。

类型0软件、1编译时间、2 CPUID、3硬件、4模块SN、5PCB SN、6 BMS产品文本、7 APP CRC32（8位大写十六进制ASCII）。SN/产品是12个参数字的小端原始内存，保留wire字节顺序。

`direct_bms_identity`严格检查地址、命令、类型、长度、CRC、字段字符及填充；失败不修改快照。唯一BMS UART任务完成正常采集后，每周期最多查询一个类型。仅USB命令7显式触发，开机不自动查询。

工具返回valid_mask、进度和年龄。无响应为UNSUPPORTED_OR_NO_REPLY，部分响应为PARTIAL；所有结果都返回upgrade_authorized=false。

所给Boot Keil工程未编入PC_485_protocol.C与afe_sample_task.c。源码存在不证明设备当前APP支持。不能把源码内BOOT.001.1.1及硬件常量冒充设备读回；软件文本也不自动确认新APP已运行。

## BMS 校准：封帧和ACK已实现，设备写入未启用

| 命令 | PC_485_protocol.C位置 | 功能/单位 |
|---|---|---|
| 0A | 843–905 | 第一至第N节双点电压；参考mV |
| 0B | 909–964 | 充电电流双点；参考单位100mA |
| 0C | 967–1021 | 放电电流双点；参考单位100mA |
| 0D | 只有define，无处理分支 | 不支持总电压专用校准 |

CELL_VOL_NUM为运行参数，此AFE/校准槽最多16节。codec拒绝未知串数、超出已确认串数、无效点/范围及0D。单体接受1–3999mV；电流wire为uint16，工装必须另行限定物理范围，不能把wire最大值当安全试验上限。

ACK严格匹配类型、节数、参考值、长度、CRC及result。result=1仅代表参数存储成功，不代表精度或出厂通过。

系数生效：afe_sample_task.c:46–90计算y=k*x+b；109应用单体；174给出电流缩放；180、193应用充放电电流。

读回源码：PC485命令03，PC_485_protocol.C:320–365。请求start_pid:uint16LE+para_num:uint16LE；响应start_pid,para_num,result:uint8,value[para_num]:uint16LE。每次1–300参数，必须满足para_valid。PID2000–2063为16节四参数（真实低、原始低、真实高、原始高），2064–2067充电，2068–2071放电（parameter.h:18,194–202）。本版尚未接入参数读回。

实际APP能力、仪器参考、稳定采样/低高点顺序、维护条件、参数读回和最终精度仍需完成。本版纯codec没有UART校准写入入口。

## DSP：停止轮询不等同停机

App的UsartDsp.c:621–623仅在升级标志置位时停查询；Boot(9)/Boot/source/Collector/Update.c:20–37直接42→HR→擦写→EOT。未发现主动停机命令、维护完成ACK或已定义的维护态。

SystemManage.h:317–364、UsartDsp.c:238–240给出可观察状态：status0 bit6 TrigSig、bit13 EnDAB、bit14/15充放电；status1 bit7 PowerSwitch、bit8 EpsSwitch、bit9 BrgPwmEnable。CMD04有功率/电流。可作被动证据，不能自行推导完整维护授权。

DSPVersion1–4在所给App中未找到实际接收赋值，Collector常量映射不能当版本读回。DSP校准在UsartDsp.c:320–333被注释；ARM Vpv/Vout gain/offset只有保存/default，未找到DSP生效链。

## 旧工作链迁移：不能把新增接口当作原升级前提

用户已确认 `esp32c3_l10_idf` 配合 ARM 可完成 BLE/MQTT DSP/BMS 升级。新程序保留源文件的目标 Boot 事务，去掉 ESP→ARM Collector 外层；完整下载/大小/SHA 校验通过后，交由目标唯一 UART owner，轮询与配置写暂停，事务出口恢复。不会调用旧 ARM UART 转发实现。

BMS `BmsUpdate.c:281–289` 发 `FF 00 00 00 00 00 00 00`；`update_protocol.C:213–233` 仅双方值非零时匹配，275–276 的硬件高低字节回显请求，不是目标硬件读回。直连兼容入口沿用全零匹配，不能宣称这个回复确认了实际硬件。FC 回复来自 Boot 全镜像检查；ACK失败、Boot错误、校验不通过不会报告成功。

DSP 遵循原 HR→擦除ACK/CR→数据块CR→EOT。源码没有独立最终运行版本接口；界面/上报明确说明 Boot 传输完成、版本未读回。停止日常串口轮询不等于功率级停机；本版不伪造新的停机确认，也不把源链缺少的接口新增为传输前置条件。下载100%不等于写入完成。

这是源协议迁移与主机回归证据，不替代实际DSP/BMS升级、恢复与功率级验收。
