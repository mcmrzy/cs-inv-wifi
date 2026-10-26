# 0.2.0-direct-ui 原 ARM 功能迁移清单

源基线为 App(10)/App/source。保持独立 ESP32-C3 工程、无 CS GPIO、已有 WiFi/MQTT/BLE/OTA 的 ESP 侧能力。没有烧录到本机或远端设备；实机通信、按键和屏幕验收仍需烧录后日志及观察。

## 原程序对照

| 原 ARM 模块 | 新实现 | 状态 |
|---|---|---|
| Keyboard/keyboard.c | direct_keys + direct_key_codec | BS818A 16位LSB、25us、独立10ms扫描、200ms重复、组合键；采集时间戳保留，刷屏不阻塞扫描 |
| ComDSP/UsartDsp.c 命令03/04 | direct_poll + direct_protocol | 三组状态/告警、模式、故障、电压/电流/功率/频率/温度；保持原单位和符号 |
| ComDSP/UsartDsp.c 命令25 | 84项配置快照及有效标记 | 只标识DSP实际提供的字段；未知显示-- |
| MainForm | direct_panel_runtime + direct_ui | 输入/PV/电池/输出能流文字、充放电和旁路、温度、模式、故障、时间；第二显示模式；布局重新实现，非像素复刻 |
| GUI_FontHZ14x14 | direct_ui_assets | 原96个ASCII字形；原EN_CHINESE_STR=0，保留英文界面 |
| SystemMenu / Input / Output / Charge / Battery / Generator / Other / Eqlzt | direct_ui分组参数页 | 15个可达菜单入口，全部84项可浏览；可写项见下一节，未映射项只读 |
| ParamForm | 4页参数详情 | 输入/PV/母线/输出、逆变/电池、ESP当日/累计电量、通信诊断 |
| AlarmForm | 原64警告、32故障、37BMS文本表 | 三组DSP告警共48位可达；原第四警告字未由DSP03提供。BMS状态故障位当前协议未解析；不可假造 |
| BatteryMonitoringForm | BMS电池监控3页 | 总压、电流、SOC/SOH、单体最小/最大/差值、16单体、告警保护；无回复显示离线。循环次数、剩余容量、满充容量及温度未在当前BMS codec提供，未假造 |
| HelpForm | 设备信息 | ESP实际版本、SN、通信/触摸/屏幕接线；没有固定旧ARM版本冒充ESP版本 |
| RealTimeForm | UTC转换 + ESP时钟 + NVS时区 | 年月日时分秒及UTC偏移；规避原SNTP进程TZ=CST-8导致重复减8小时。无电池RTC，断电后需重新校时 |
| Buzzer/Blank | ESP本地UI NVS | 蜂鸣器默认关闭，按键/新报警有限时70ms提示，报警控制只作用ESP提示；Screen Blank用DISPOFF/DISPON，外部背光仍亮 |
| Restart / FactoryReset | 明确Yes/No后执行 | Restart重启ESP；Reset只重置ESP界面偏好和时区，保留DSP功率参数、配网及电量。不能声称已恢复DSP出厂配置 |
| EnergySatistics | direct_energy + codec | PV、市电充放、电池充放、输出6通道，独立1秒任务，Wms积分，仅新鲜04数据；NVS每10分钟和换日保存 |

## 参数写入

菜单在读取到真实参数并确认DSP在线后才允许编辑。左右键改草稿、ENTER提交、ESC取消。发送由唯一DSP任务执行；只收到命令ACK不能说明应用成功，需命令25同值回读，5秒未确认显示超时。UI与云端共享队列，但结果分离，后续云端写入不覆盖屏幕结果。

已支持原有效发送路径：

- 07：输出电压（DSP 0.1V；菜单以V显示）。
- 08：输出频率（DSP 0.01Hz，5000/6000）。
- 15：电池类型及配对协议；切换类型后旧电压profile标记失效。
- 16：返回市电/电池电压。
- 17：总充电、市电充电电流，保留另一个已知值。
- 18：输出来源、充电来源（原优先值1/3/4）。
- 24：SOC回市电/回电池/截止。
- 2F：当前电池类型的bulk/float/cutoff电压，保留其他已知电压；不改未知或非当前profile。

本地可写：索引14报警声音提示、15屏幕内容熄屏、41蜂鸣器；这些显示Saved on ESP controller，不冒充DSP设置生效。

## 尚受原代码或硬件限制的内容

- DSP25是否主动发送、命令回读是否实际对应，需要实机确认；未收到时显示未知，不用默认00/01/02覆盖运行配置。
- 00/01/02完整参数块还依赖原ARM EEPROM/配置表中的configWord、并机及发电机等字段。现有DSP25没有提供全部值，不能从零重建并声称等价。
- 原CMD19输入范围、28均衡、90时间发送代码处于注释；28长度与CRC还不一致。本版显示其菜单/真实读值，但不把注释块当有效控制协议启用。
- 过载/过温重启、源中断告警、并机配置等单独写路径或回读尚未完整接入，菜单显示只读。
- 发电机启停依赖原ARM PC2/PC3。本板ESP接线没有对应已确认输出，不执行启停。DSP04无发电机独立功率字段，发电机电量显示未计量。
- 原EDS/智能插座菜单入口及多处控制流程已注释；本版All parameters保留索引浏览，不宣称能配对或控制插座。
- 旧ARM历史电量无法恢复。新计量仅自ESP安装开始；完整4MB烧录将清除新旧NVS内容。断电可能丢失最近未保存的最多约10分钟计量。
- 背光外部供电，软件无法关闭实际LED；GPIO9为RS/D-C，原理图MISO名称不表示读数据。

## 接线和使用

CS→GND；SCL→GPIO6；SDA→GPIO7；RS/D-C→GPIO9；RESET→GPIO10；BS818A CLK→GPIO2、DAT→GPIO3；DSP TX→GPIO1、RX→GPIO0；BMS TX→GPIO21、RX→GPIO20。USB串口保留GPIO18/19。GPIO8蜂鸣器保持启动低电平。

主界面：ENTER设置，RIGHT报警，LEFT两次700ms内进参数，UP/DOWN切换主显示模式，ESC返回。参数页左右切页；设置页上下选行、左右修改、ENTER保存。ENTER+BACK重复第5次复位屏幕、第11次重启ESP。无CS写入时不读屏、不探测GPIO，软件SPI时序维持之前无CS候选方案；该接法仍未实机确认。

测试：protocol_test、key_codec_test、ui_test、energy_test均通过主机GCC -Wall -Wextra -Werror；ESP-IDF整体编译通过。测试验证软件协议和状态机，不证明实际DSP采样、BMS、屏幕或功率系统已工作。
