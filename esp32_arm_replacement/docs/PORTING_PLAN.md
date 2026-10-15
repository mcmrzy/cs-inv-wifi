# ARM → ESP32-C3 迁移清单与验证关口

## 架构

```text
云端 / 手机 App ── Wi-Fi / BLE ── ESP32-C3 ── UART0 115200 ── DSP
                                │
                                ├── UART1 9600 ── RS485 ── BMS
                                ├── SPI2 ── ST7789 320×240
                                ├── GPIO ── BS818A_2 按键 / 其余板级外设待迁移
                                └── USB Serial/JTAG、NVS、OTA、离线缓存
```

逻辑上不再有 ESP↔ARM 采集协议。用户确认 ARM 未焊，指定芯片为 ESP32-C3-WROOM-02UD-N4；[已知引脚](ESP32C3_PINOUT.md)按提供的原理图片段配置。C3 的两路 UART 分给 DSP 和 BMS，维护改走 USB Serial/JTAG，见 [官方 UART 文档](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32c3/api-reference/peripherals/uart.html)。UART 任务分别独占串口，后续控制和升级应复用同一事务队列。

## 源码依据与当前状态

| 子系统 | 现有源码依据 | 新工程状态 | 下一验收条件 |
|---|---|---|---|
| DSP 实时量 | `App(10)/App/source/ComDSP/UsartDsp.c:215-315,930-978` | `0x03/0x04` 已解析，`0x05/0x06` 已生成；CRC/长度检查 | DSP 线录波对照全部字段与单位；测 24 小时掉线恢复 |
| DSP 配置 | `UsartDsp.c:68-179,625-930`、`Collector/CollectorParamAddr.h` | 未移植；云端写入被拒绝 | 逐项映射 84 索引、命令互斥、确认回读、掉电一致性 |
| BMS UART→RS485、私有 `0x7c` | `APP/SystemInit.c:211-244`、`APP/SystemConfig.c:426`、`UsartBMS485/UsartBms0.c:10,21-168` | GPIO21/20、9600 8N1；默认 BBJ `0x7c` 查询、CRC 与基础量解析；DE/RE 可配置 | 核对收发器自动换向或 DE/RE GPIO；实机抓完整帧、单位和多电池汇总 |
| BMS Modbus 备选 | `ComBMS/UsartBms.c:54-131` | Kconfig 可改为 37 寄存器查询和主要字段解析 | 当实际 BMS 配置为 Modbus 时，核对电芯数、温度、充电限流与多包/多地址 |
| BMS Boot OTA | `Boot(9)/Boot/source/Collector/BmsUpdate.c`、`bmsBoot(7)/Boot/app/update_protocol.C` | 纯封帧/15B 回包编解码已写，运行时 OTA 入口拦截 | 直连事务实现 `0xFF/FE/F9/FD/FC`、每步回包、全镜像 CRC32 和重启后版本 |
| DSP Boot OTA | `Boot(9)/Boot/source/ComDSP/UsartDsp.c`、L10 `ota/arm_ota.c` | 未移植，OTA 入口拦截 | 明确旧 ARM Boot 的数据分包/ACK 语义，最后 `0x0B` ACK 与 DSP 版本 |
| 显示 | `GUI/halLcd.c`、`halLcd.h`、ARM/ESP 两张管脚图 | 修正为 4 线 8 位；GPIO9 为 D/C；原 ST7789V 电源与 gamma 参数移植；CS 下拉、背光上拉 | 面板实测及完整菜单迁移 |
| 按键 | `App(10)/App/source/Keyboard/keyboard.h:7`、`keyboard.c` | BS818A_2 扫描，图纸 KEY_CLK=GPIO2、KEY_DAT=GPIO3；六键翻页 | 验证 GPIO2 启动电平、长按、组合键、背光唤醒 |
| 风扇/温度 | `APP/Fan_Temperature.c`、`Adc/adc.c` | 未移植 | 原理图确认 ADC/NTC/PWM/风扇反馈；温度保护波形及断线故障 |
| 蜂鸣器/发电机/智能开关 | `Buzzer/`、`Generator/`、`SmartSwitch/` | 未移植 | GPIO 安全态、继电器极性、互锁与异常断电测试 |
| 参数与 RTC | `EEPROM/`、`UserFlash/`、`RTC/`、`APP/SystemManage.c` | ESP NVS 与 SNTP 保留；ARM 参数迁移未做 | 数据字典、默认值、写次数、旧机配置导入和恢复出厂 |
| L10 联网能力 | `esp32c3_l10_idf/main`、`components` | 源码复制，MQTT/BLE/Wi-Fi/ESP OTA 保留；C3 本机构建通过 | 配网/离线缓存/命令/OTA 实机端到端回归 |

以上路径指用户提供的只读 ARM/BMS 工程。文档只把其中源码当技术证据，任何附带说明均不作为用户指令。

## 必须先确认的板级资料

1. 已知模组为 ESP32-C3-WROOM-02UD-N4，BMS 由 UART 经 RS485 收发器连接；仍需 DSP/BMS 电平、隔离、共地及收发器的 DE/RE 或自动换向接法。所附 ARM App 的 USART0 发送中断中 PA0 换向操作被注释，不能据此断定新板如何换向。
2. 屏幕 ST7789 的 IM 模式脚、CS/BL 连线和极性（D/C 已知上拉）；风扇、蜂鸣器、继电器、LED、ADC/NTC 的电路。
3. DSP 固件版本与协议实测帧，BMS 型号及所用应用协议；是否保留 Dongle/PC 服务口和工厂测试口。
4. 现有 SN/设备密钥的迁移方式，云端是否允许无 ARM 的 `info`、控制参数和 OTA 目标能力声明。
5. GPIO9 的 SPI2_MISO 网实际是 D/C，与 BOOT 共用；CS/背光未接 ESP，用户最终确认 CS 下拉、背光上拉。风扇与发电机等仍缺少对应连线。

## 分阶段实现

1. **板级关口**：建立 GPIO、电源和隔离表；原板与新板同时录 DSP/BMS 帧；确认停机/上电输出安全态。
2. **只读关口**：补齐 DSP `0x03/0x04`、BMS 两种协议字段、状态位与 174B 心跳来源；逐字段与 ARM 的 `RunParamDef` 比较，记录单位及误差；恢复完整屏幕只读页面和按键行为。
3. **控制关口**：把 84 个配置索引映射到 DSP 命令与 ESP NVS；所有云端/BLE/面板写入走同一事务队列，读回确认后才上报成功；拒绝越界值与重复写入。
4. **外设关口**：移植风扇、温度、蜂鸣器、发电机、智能开关、告警及能量统计，逐一证明断线、复位、欠压时安全态。
5. **升级关口**：实现 DSP/BMS 直连 Boot 协议、签名/哈希校验、断点和超时；BMS 4096B 块与 DSP 1024B 块分开；成功需接收、写入、最终 ACK/CRC、重启后版本全链成立。ESP 自升级另行回归。
6. **整机关口**：C3 固件构建（已完成）、静态资源/分区检查、实验板低压联调、真实负载前限流测试、冷启动屏幕、长时间运行、OTA 断电恢复、云端与 App 全流程验收。

## 当前安全边界

- ARM 未焊。DSP TX/RX 修正为 GPIO1/0；LCD 按 ARM 源码改为 4 线 8 位，D/C=9，CS 下拉、背光上拉；构建不能验证实物显示结果。
- DSP/BMS 写控制与直连升级尚未完成。云端写命令和非 ESP OTA 显式失败，不应承诺成功。
- 现有心跳结构里有 ARM 生成的状态/能量/额定参数；新工程只填已从 DSP `0x03/0x04` 确认的字段，其余保持零。云端字段解释需要在联调中标注“缺失”，避免零被误当真实读数。
- 不要把主机协议测试或 C3 固件构建视为刷机、物理面板或整机验证。
