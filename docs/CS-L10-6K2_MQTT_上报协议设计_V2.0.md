# CS-L10-6K2 逆变器 ESP32→云端 MQTT 上报协议设计 V2.0（修订版）

> **文档版本**: v2.0
> **适用产品**: CS-L10-6K2 48V 单相离网逆变器（GD32F30x 系统）
> **适用对象**: ESP32 固件开发（esp32c3_l10_idf）、云端（device-communication / business-api / 管理后台）开发
> **最后更新**: 2026-08-03
> **上游协议**: `collector_protocol_print.html`（辰烁采集器通信协议规范，ARM↔ESP32 UART）
> **协议基线**: `辰烁科技48V离网逆变器遥测协议与存储重设计V1.0.26.713.html`（全系统 MQTT 协议基线，本协议为其在 CS-L10-6K2 型号上的落地实现）
> **原始设计稿**: `CS-L10-6K2_MQTT_上报协议设计.md` v1.0（本修订版据此完善，修订对照见附录 A）

---

## 目录

1. [概述](#1-概述)
2. [与既有协议的关系](#2-与既有协议的关系)
3. [采集器协议摘要（UART 层）](#3-采集器协议摘要uart-层)
4. [主题与上报频率](#4-主题与上报频率)
5. [上行信封](#5-上行信封)
6. [V2 心跳定义](#6-v2-心跳定义)
7. [设备信息 info](#7-设备信息-info)
8. [告警事件 alarm](#8-告警事件-alarm)
9. [配置上报 config](#9-配置上报-config)
10. [网络状态 status / LWT](#10-网络状态-status--lwt)
11. [控制命令](#11-控制命令)
12. [服务器解析链路](#12-服务器解析链路)
13. [数据库型号配置](#13-数据库型号配置)
14. [字段优化对照表](#14-字段优化对照表)
15. [实施清单与验证](#15-实施清单与验证)
- [附录 A：与原始设计稿修订对照](#附录-a与原始设计稿修订对照)

---

## 1. 概述

CS-L10-6K2 是辰烁科技 48V 单相离网逆变器，ARM 主控（GD32F30x）通过 USART 与 ESP32 采集器通信（PPP 帧 + TEA 双重加密 + CRC32，地址映射式读写）。ESP32 负责将 ARM 数据经 MQTT 上报云端，并转发云端控制命令。

本协议将采集器协议中 70 个运行参数收敛为 **49 个位置数组值**（V2 心跳），与服务器现有 V1 心跳（`device-communication/internal/telemetry/heartbeat_v1.go`）保持同一套「位置数组 + 版本号」模式：**同一 heartbeat 主题，服务器按 `v` 字段分派**，存储/缓存/展示链路完全复用。

### 1.1 设计原则

| 原则 | 说明 |
|------|------|
| 精简 | 采集器协议无数据源的字段不上报（cells 电芯、mos/ambient 温度、风扇转速、运行小时）；ARM 已给出总量的字段不拆分（Ppv 总功率直接上报） |
| 位置数组 | 组结构与现有 `device_protocol_fields`（group_code + field_index）体系一致，数组长度、下标、类型和单位永久冻结 |
| 版本化 | `v=2` 区分协议版本，V1 设备与服务器 V1 解析路径不受影响 |
| 原始量纲 | 心跳上报 ARM 协议原始整数（含 0.1 缩放），服务器按字段定义还原，避免浮点误差 |
| 元数据驱动 | 字段定义、显示配置、控制命令全部走数据库配置，不改代码 |
| 单一快照 | 整机实时数据统一由 heartbeat 上报（180s），不拆分多主题 |

### 1.2 与 V1.0.26.713 的字段取舍

V1.0.26.713 的 V1 心跳数组（`ac[8]/bat[23]/pv[7]/sys[11]/eng[12]/cells[2]`）为通用型号设计，其中多项在 L10 采集器协议中无数据源，本 V2 按型号能力裁剪：

| V1 通用字段 | L10 处理 | 原因 |
|------------|---------|------|
| `cells[2]` 电芯电压/温度 | **剔除** | 采集器协议无电芯数据（无 BMS 直连） |
| `mos_temperature` / `ambient_temperature` / `fan_speed` / `runtime_hours` | **剔除** | 协议无对应地址 |
| `battery_soh` / `cycle_cnt` / 容量 / 单体电压 / BMS 电流电压请求等 | **剔除** | BMS 通信字段，协议无数据源 |
| 充电/放电能量分离（`daily_charge` vs `daily_discharge`） | **保留并扩展** | L10 协议分别提供 |
| `work_state`（枚举） | **服务端派生** | 由 `sys_status` 位组合推导，枚举值与 V1 完全一致 |

---

## 2. 与既有协议的关系

| 文档 | 角色 |
|------|------|
| `辰烁科技48V离网逆变器遥测协议与存储重设计V1.0.26.713.html` | **全系统协议基线**：信封 `{t,v,data}`、主题命名、QoS、存储设计、命令信封、告警字典规则均以其为准 |
| `CS-L10-6K2_MQTT_上报协议设计.md` v1.0 | 本协议原始设计稿（已并入本修订版） |
| 本文档 v2.0 | **CS-L10-6K2 型号协议正式版**：定义 L10 心跳 schema（v=2）与 L10 特有字段 |

- L10 设备上行 **heartbeat 主题与 V1 相同**（`cs_inv/{sn}/heartbeat`），通过信封 `v=2` 与 V1 区分，服务器解析器按 `v` 分派到 `ParseHeartbeatV2`。
- L10 设备的 `info` / `alarm` / `config` / `status` / `cmd` / `cmd/response` / `ota/*` 与 V1 完全同构（同信封、同字段语义），仅 `data` 内容按本协议定义。
- 旧 Topic（`data/ac`、`data/pv`、`data/battery`、`data/status`、`data/energy`）**禁止新设备发送**，只允许服务端兼容层接收。

---

## 3. 采集器协议摘要（UART 层）

### 3.1 物理层与帧格式

| 项 | 值 |
|----|----|
| 接口 | USART 串口，115200 bps / 8N1，无流控 |
| 帧定界 | PPP 封装 `0x7E ... 0x7E`（0x7E→0x7D 0x5E，0x7D→0x7D 0x5D） |
| 加密 | TEA 双重加密（FrameKey + 动态 TempKey），128 位密钥，CBC（InitV=0） |
| 校验 | CRC32（帧尾 4B，逐 32 位字，poly 0x4C11DB7，无反射） |
| 字节序 | 小端序，最大帧长 1088 字节 |

帧结构：`Header(0xAA) + CmdLen(2B, 4 的倍数) + CMD(1B) + Param1(2B 起始地址) + Param2(2B 数据长度) + Data[] + CRC32(4B)`

### 3.2 命令集

| 码 | 名称 | 用途 |
|----|------|------|
| 0x01 | READ_CTRLPARAM | 读控制参数（0x0000-0x0053） |
| 0x02 | WRITE_CTRLPARAM | 写控制参数 |
| 0x03 | READ_RUNPARAM | 读运行参数（0x03E8-0x0442，176B） |
| 0x04 / 0x05 | READ/WRITE_R_W_DATA | 读写可读写数据（0x00C8-0x00CB：CtrlParamAlterTime、UTC） |
| 0x06 | READ_R_DATA | 读只读数据（0x0190-0x01A9：版本/SN/额定值，52B） |
| 0x07 | CTRLCMD | 控制命令（预留） |
| 0x14 | READ_TEST | 读测试参数（单位与运行参数不同） |

### 3.3 参数地址空间

| 区域 | 地址 | 数量 | 说明 |
|------|------|------|------|
| 控制参数 | 0x0000-0x0053 | 84 字节 / 42 个 u16 | 输出优先级、充电电流、电池容量/类型、电压频率、主从、SOC 阈值、均衡、发电机、时间等 |
| 可读写 | 0x00C8-0x00CB | 4 字节 | CtrlParamAlterTime（u32）、UTC 时间（u32，写入自动校时） |
| 运行参数 | 0x03E8-0x0442 | 176 字节 / 88 个 u16 | 见第 14 节字段优化对照表 |
| 只读 | 0x0190-0x01A9 | 52 字节 / 26 个 u16 | ARM/DSP 版本、模块号、SN（4×u32，16B ASCII）、硬件版本、额定参数、Bootloader 版本 |

### 3.4 系统状态位（SysStatus，运行参数偏移 0）

| 位 | 名称 | 位 | 名称 |
|----|------|----|------|
| 0 | StandBy 待机 | 6 | GenCharging 发电机充电 |
| 1 | Fault 故障 | 7 | ACBypass AC 旁路 |
| 2 | Charge 充电 | 8 | ToLoad 输出负载 |
| 3 | Discharging 放电 | 9 | Pvinput PV 输入 |
| 4 | PVCharging PV 充电 | 10 | AcInput AC 输入 |
| 5 | ACCharging AC 充电 | 11 | GeInput 发电机输入 |

---

## 4. 主题与上报频率

沿用 `cs_inv/{sn}/...` 主题体系，所有上行消息使用统一信封：

| 主题 | QoS | Retain | 频率 | 来源 | 说明 |
|------|-----|--------|------|------|------|
| `cs_inv/{sn}/heartbeat` | 1 | false | **180s** + 启动立即 + `query_telemetry` 补报 | ARM | **V2 心跳（本协议核心）** |
| `cs_inv/{sn}/info` | 1 | false | 连接时、信息变化、`query_info` 后 | ARM 只读 | 设备能力与固件信息 |
| `cs_inv/{sn}/alarm` | 1 | false | 发生、级别变化、恢复（不等待心跳） | ARM | 告警事件闭环 |
| `cs_inv/{sn}/config` | 1 | false | 连接时、配置生效后、`query_config` 后 | ARM 控制参数 | 设备实际生效配置（阶段 2 实现） |
| `cs_inv/{sn}/status` | 1 | true | 上线一次 + LWT | ESP32 自动 | 网络在线状态（online + rssi + ip），不周期刷 |
| `cs_inv/{sn}/cmd` | 1 | — | 按需（云端→ESP32） | 云端 | 控制命令下行 |
| `cs_inv/{sn}/cmd/response` | 1 | false | 按需（ESP32→云端） | ESP32 | 命令执行结果上行 |
| `cs_inv/{sn}/ota/cmd`、`ota/status` | 1 | — | 按需 | — | OTA（沿用现有） |

> **时间戳**：所有上行数据由 ESP32 添加 Unix 秒级时间戳（NTP 同步），与现有实现一致。
> **上报失败**：不得补发已过期的实时快照（heartbeat 可丢弃）；告警、命令结果、配置状态必须保留到确认投递（QoS 1 + 离线缓存）。

---

## 5. 上行信封

所有设备上行消息统一使用外层信封（V1.0.26.713 3.2 节）：

```json
{
  "t": 1783676930,
  "v": 2,
  "data": { }
}
```

| 字段 | 类型 | 必填 | 定义 |
|------|------|------|------|
| `t` | int64 | 是 | 业务采样或事件生成时间，Unix 秒，UTC；除 LWT 外必须 > 0 |
| `v` | uint16 | 是 | 协议主版本，本协议固定为 `2`；属于信封元数据 |
| `data` | object | 是 | 当前 Topic 的业务对象；时间和协议版本不得放入此对象 |

约束：

- 编码必须为 UTF-8 JSON；禁止 `NaN`、`Infinity` 和字符串形式的数值。
- `t`、`v` 是信封元数据；`data` 只保存业务字段。所有上行 Topic 均使用相同字段位置。
- 固定数组的长度、下标、数据类型和单位永久冻结。暂不可用的值必须填 `null`，不得删除元素；数值 `0` 是有效业务值。
- 实时遥测使用**原始量纲**（含 0.1 缩放，见 6.1 单位列）；控制命令参数使用整数缩放值，缩放规则由命令定义。
- SN 只从 Topic 获取，不在 `data` 中重复发送。
- V2 不使用全局消息序号；重复投递由服务端按 `sn + topic + t + data_hash` 在短窗口内消除（`data_hash` 由服务端计算，设备不发送）。

---

## 6. V2 心跳定义

### 6.1 总览

```json
{
  "v": 2,
  "t": 1783000000,
  "data": {
    "sys": [11],
    "pv":  [5],
    "ac":  [11],
    "chr": [3],
    "bat": [5],
    "eng": [14]
  }
}
```

- 各分组为**定长位置数组**，元素必须是数值或 `null`（`null` 表示该字段本次无数据，服务器标记 QualityPartial）。
- 上报值为 ARM 协议原始整数（已含缩放），**负数直接使用 JSON 负数**（如电池放电电流 -255 = -25.5A）。
- 与 V1 不同，V2 **不含 cells 组**、不含 mos/ambient 温度（采集器协议无电芯数据）。

### 6.2 组字段顺序定义（服务器解析依据）

#### sys（11 个）— 系统状态

| 索引 | 字段键 | 说明 | 单位 | 范围 | ARM 来源 |
|------|--------|------|------|------|---------|
| 0 | `sys_status` | 系统状态位（12 位 bitmask，见 3.4） | - | 0-4095 | SysStatus |
| 1 | `fault_code` | 故障码 | - | 0-2^32 | FaultValue |
| 2 | `warning` | 告警位（64 位） | - | 0-2^64 | Warning |
| 3 | `bms_warning` | BMS 告警 | - | 0-65535 | BmsWarning（ARM 为 u16） |
| 4 | `inverter_temperature` | 逆变温度 | 0.1°C | -40-100 | InvertTemp |
| 5 | `boost_temperature` | Boost 温度 | 0.1°C | -40-120 | BoostTemp |
| 6 | `transformer_temperature` | 变压器温度 | 0.1°C | -40-120 | TransformerTemp（ARM 未赋值 → null） |
| 7 | `pv_temperature` | PV 温度 | 0.1°C | -40-120 | PvTemp（ARM 未赋值 → null） |
| 8 | `dc_bus_voltage` | 母线电压 | 0.01V | 0-500 | BusVolt |
| 9 | `load_percent` | 负载百分比 | 0.1% | 0-120 | LoadPercent |
| 10 | `battery_overcharge` | 过充标志 | - | 0-1 | BatOverCharge |

#### pv（5 个）— 光伏

| 索引 | 字段键 | 说明 | 单位 | 范围 | ARM 来源 |
|------|--------|------|------|------|---------|
| 0 | `pv1_voltage` | PV1 电压 | 0.1V | 0-150 | Vpv1 |
| 1 | `buck1_current` | Buck1 电流 | 0.1A | 0-30 | Buck1Curr（ARM 当前固定 0） |
| 2 | `pv2_voltage` | PV2 电压 | 0.1V | 0-150 | Vpv2 |
| 3 | `buck2_current` | Buck2 电流 | 0.1A | 0-30 | Buck2Curr（ARM 当前固定 0） |
| 4 | `pv_total_power` | PV 总功率（ARM 直接给出，不再拆分） | 0.1W | 0-7500 | Ppv |

#### ac（11 个）— 交流

| 索引 | 字段键 | 说明 | 单位 | 范围 | ARM 来源 |
|------|--------|------|------|------|---------|
| 0 | `ac_output_voltage` | AC 输出电压 | 0.1V | 0-250 | ACOutputVolt |
| 1 | `ac_output_frequency` | AC 输出频率 | 0.01Hz | 0-55 | ACOutputFreq |
| 2 | `output_power` | 输出有功功率 | 0.1W | 0-7500 | OutputWatt |
| 3 | `output_apparent_power` | 输出视在功率 | 0.1VA | 0-7500 | OutputVA |
| 4 | `output_current` | 输出电流 | 0.1A | 0-100 | OutputCurr |
| 5 | `grid_voltage` | 电网电压 | 0.1V | 0-300 | GridVolt |
| 6 | `grid_frequency` | 电网频率 | 0.01Hz | 0-55 | GridFreq |
| 7 | `ac_input_power` | AC 输入功率 | 0.1W | 0-7500 | ACInWatt |
| 8 | `ac_input_apparent_power` | AC 输入视在功率 | 0.1VA | 0-7500 | ACInVA |
| 9 | `ac_discharge_power` | AC 放电功率 | 0.1W | 0-7500 | ACDisChrWatt |
| 10 | `ac_discharge_apparent_power` | AC 放电视在功率 | 0.1VA | 0-7500 | ACDisChrVA |

> **命名勘误**：v1.0 稿的 `ac_bypass_power` 实为采集器协议 `ACDisChrWatt`（交流放电功率），本版更名为 `ac_discharge_*`。
> **单位勘误**：采集器协议文档中 `ACDisChrWatt` 单位标注为 `0.1kWh`，实为功率字段，应为 `0.1W`（与 ACDisChrVA 0.1VA 对应）。本协议按 `0.1W` 处理。
> **数据源同值注记**：ARM 上 `ACInWatt/ACDisChrWatt` 与 `ACChrWatt` 同值（均取 Pbat），充电/放电/输入语义需结合 `sys_status` 位（bit2 Charge / bit3 Discharging）区分，服务端不可单独用 ac[7]/ac[9] 判断充放方向。

#### chr（3 个）— AC 充电

| 索引 | 字段键 | 说明 | 单位 | 范围 | ARM 来源 |
|------|--------|------|------|------|---------|
| 0 | `ac_charge_power` | AC 充电功率 | 0.1W | 0-7500 | ACChrWatt |
| 1 | `ac_charge_apparent_power` | AC 充电视在功率 | 0.1VA | 0-7500 | ACChrVA |
| 2 | `ac_charge_current` | AC 充电电流 | 0.1A | 0-150 | ACChrCurr |

#### bat（5 个）— 电池

| 索引 | 字段键 | 说明 | 单位 | 范围 | ARM 来源 |
|------|--------|------|------|------|---------|
| 0 | `battery_voltage` | 电池电压 | 0.01V | 0-70 | BatVolt |
| 1 | `battery_soc` | 电池 SOC | **1%** | 0-100 | BatterySOC（ARM 为整数 %） |
| 2 | `battery_current` | 电池电流（**充电为正，放电为负**） | 0.1A | -150-150 | SysStatus 位 2/3 选择 BatChgCurr / -BatDischgCurr |
| 3 | `battery_charge_power` | 电池充电功率 | 0.1W | 0-7500 | BatChrWatt |
| 4 | `battery_discharge_power` | 电池放电功率 | 0.1W | 0-7500 | BatDisChrWatt |

> v1.0 稿的 `bat[5] battery_overcharge` 与 `sys[10]` 同源冗余，本版**删除**，统一取 `sys[10]`；`battery_overcharge` 字段目录仅保留 system 组一处。
> **数据源同值注记**：ARM 上 `BatDisChrWatt` 与 `BatChrWatt` 同值（均为 Pbat），充/放功率语义须由 `sys_status` 位或 `bat[2]` 电流符号决定，服务端不可同时使用 `bat[3]`、`bat[4]` 求和。
> `battery_power`（服务端派生列，**充电为正**）：SysStatus 位 2（Charge）→ `+bat[3]`；位 3（Discharge）→ `-bat[4]`。

#### eng（14 个）— 能量统计

| 索引 | 字段键 | 说明 | 单位 | 范围 | ARM 来源 |
|------|--------|------|------|------|---------|
| 0 | `gen_energy_daily` | 发电机今日发电 | 0.1kWh | 0-4.29e9 | EGen_today |
| 1 | `gen_energy_total` | 发电机总发电 | 0.1kWh | 0-4.29e9 | EGen_total |
| 2 | `pv_energy_daily` | PV 今日发电 | 0.1kWh | 0-4.29e9 | Epv_today |
| 3 | `pv_energy_total` | PV 总发电 | 0.1kWh | 0-4.29e9 | Epv_total |
| 4 | `ac_charge_energy_daily` | AC 今日充电 | 0.1kWh | 0-4.29e9 | Eac_chrToday |
| 5 | `ac_charge_energy_total` | AC 总充电 | 0.1kWh | 0-4.29e9 | Eac_chrTotal |
| 6 | `battery_discharge_energy_daily` | 电池今日放电 | 0.1kWh | 0-4.29e9 | Ebat_dischrToday |
| 7 | `battery_discharge_energy_total` | 电池总放电 | 0.1kWh | 0-4.29e9 | Ebat_dischrTotal |
| 8 | `battery_charge_energy_daily` | 电池今日充电 | 0.1kWh | 0-4.29e9 | Ebat_chrToday |
| 9 | `battery_charge_energy_total` | 电池总充电 | 0.1kWh | 0-4.29e9 | Ebat_chrTotal |
| 10 | `ac_discharge_energy_daily` | AC 今日放电 | 0.1kWh | 0-4.29e9 | Eac_dischrToday |
| 11 | `ac_discharge_energy_total` | AC 总放电 | 0.1kWh | 0-4.29e9 | Eac_dischrTotal |
| 12 | `output_energy_daily` | 输出今日放电 | 0.1kWh | 0-4.29e9 | Eop_dischrToday |
| 13 | `output_energy_total` | 输出总放电 | 0.1kWh | 0-4.29e9 | Eop_dischrTotal |

> **范围勘误**：v1.0 稿能量范围标 0-1e12，超出 ARM u32 原始值上限（0xFFFFFFFF = 4294967295），本版修正为 0-4.29e9。

### 6.3 派生字段（服务端计算，设备不上报）

| 派生字段 | 公式 | 说明 |
|----------|------|------|
| `work_state` | `sys_status` 位组合：Fault(bit1)→4、ACBypass(bit7)→2、StandBy(bit0)→0、其他→1 | 枚举与 V1.0.26.713 完全一致：0 待机 / 1 逆变 / 2 旁路 / 3 关机 / 4 故障；L10 无关机指示位，3 保留定义不使用 |
| `battery_power` | Charge(bit2)→`+bat[3]`；Discharge(bit3)→`-bat[4]` | 充电为正，放电为负（与 V1.0.26.713 `bat[4]` 语义一致） |
| `power_factor` | `ac[2] / ac[3]`（当 ac[3]>0） | 展示用，可选 |

### 6.4 完整示例

```json
{
  "v": 2,
  "t": 1783000000,
  "data": {
    "sys": [2050, 0, 32, 0, 282, 450, null, null, 41000, 624, 0],
    "pv": [1450, 82, 0, 0, 12400],
    "ac": [2205, 5002, 18703, 18756, 852, 0, 0, 0, 0, 0, 0],
    "chr": [0, 0, 0],
    "bat": [5120, 80, -255, 0, 13056],
    "eng": [0, 0, 1250, 45678, 0, 0, 1305, 23456, 0, 0, 0, 0, 1870, 98765]
  }
}
```

> 数值均为协议原始量纲（已含缩放），服务器按字段定义还原（如 `ac[0]=2205` → 220.5V、`sys[8]=41000` → 410.00V、`bat[2]=-255` → -25.5A）。

---

## 7. 设备信息 info

主题 `cs_inv/{sn}/info` · QoS 1 · 设备上线（MQTT 连接成功且已有 ARM 只读数据）、任一能力字段变化、收到 `query_info` 时上报。

```json
{
  "t": 1783676930,
  "v": 2,
  "data": {
    "model": "CS-L10-6K2",
    "manufacturer": "辰烁科技",
    "firmware_arm": "V1.1",
    "firmware_esp": "V1.1.0.L10",
    "firmware_dsp": "V2.3",
    "firmware_bms": null,
    "device_type": "off_grid_inverter",
    "phase": "single",
    "rated_power": 6200,
    "rated_voltage": 230,
    "rated_frequency": 50,
    "battery_nominal_voltage": 51.2,
    "battery_type": "LiFePO4",
    "cell_count": 0,
    "temp_sensor_count": 0,
    "inverter_module": "CS-L10-6K2",
    "hardware_version": "V1.0",
    "bootloader_version": "V1.0"
  }
}
```

| `data` 字段 | 类型 | 必填 | 单位/取值 | 定义与来源 |
|------|------|------|------|------|
| `model` | string | 是 | `CS-L10-6K2` | 型号编码，固定值 |
| `manufacturer` | string | 是 | `辰烁科技` | 固定值 |
| `firmware_arm` | string | 是 | `V主.次` | 格式化为 `V%d.%d`（ARMFirmwareVersion 0x0101 → "V1.1"） |
| `firmware_esp` | string | 是 | 版本字符串 | ESP32 固件版本（`FIRMWARE_VERSION`） |
| `firmware_dsp` | string / null | 是 | 版本字符串 | DSPFirmwareVersion 格式化，无法读取填 null |
| `firmware_bms` | string / null | 是 | — | L10 无 BMS 直连，固定 `null` |
| `device_type` | string | 是 | `off_grid_inverter` | 设备类别（与 V1.0.26.713 键名/取值一致） |
| `phase` | string | 是 | `single` | 单相机型固定值 |
| `rated_power` | uint32 | 是 | W | RateWatt |
| `rated_voltage` | number | 是 | V | NomOpVolt / 10 |
| `rated_frequency` | number | 是 | Hz | NomOpFreq / 100 |
| `battery_nominal_voltage` | number | 是 | V | NomBatVolt / 100（16S LFP 为 51.2） |
| `battery_type` | string | 是 | `LiFePO4` / `NCM` / `LeadAcid` / `Other` | 当前配置电池类型，由控制参数 0x0004 映射：0→LiFePO4、1→NCM、2→LeadAcid、其他→Other；未读取前按出厂默认 LiFePO4 |
| `cell_count` | uint16 | 是 | 节 | L10 无电芯数据，固定 `0` |
| `temp_sensor_count` | uint16 | 是 | 个 | 固定 `0` |
| `inverter_module` | string | 是 | — | 模块号（InverterModuleH/L 合并，BCD 转字符串） |
| `hardware_version` | string | 是 | — | HardwareVersion 格式化 |
| `bootloader_version` | string | 是 | — | BLVersion 格式化 |

> **修订说明**：v1.0 稿 info 无信封、含 `sn` 字段（V1.0.26.713 禁止 data 内重复 SN）、键名 `type`/`rated_freq`/`battery_voltage` 与 V1.0.26.713 不一致，本版全部修正；并补充 `device_type`/`cell_count`/`temp_sensor_count` 必填字段。

---

## 8. 告警事件 alarm

主题 `cs_inv/{sn}/alarm` · QoS 1 · 告警发生、级别变化和恢复**立即**上报，不等待 Heartbeat。

```json
{
  "t": 1783676930,
  "v": 2,
  "data": {
    "source": 0,
    "code": 8,
    "level": 2,
    "state": 1
  }
}
```

| 字段 | 取值 | 定义 |
|------|------|------|
| `source` | 0 PCS；1 BMS；2 MPPT；3 COMM | 告警来源 |
| `code` | uint32 | 型号告警字典中的代码（本协议直接上报 ARM 原始码值，字典映射由服务端完成） |
| `level` | 1 warning；2 fault | 严重程度 |
| `state` | 1 active；0 recovered | 发生与恢复闭环 |

### 8.1 L10 告警源映射（ESP32 检测规则）

| 数据源 | source | level | code | 上报时机 |
|--------|--------|-------|------|---------|
| FaultValue | 0 (PCS) | 2 (fault) | FaultValue | 0→非0 报 active；非0→0 报 recovered（code 回填最后故障码） |
| Warning | 0 (PCS) | 1 (warning) | Warning 位掩码值 | 0→非0 报 active；非0→0 报 recovered |
| BmsWarning | 1 (BMS) | 1 (warning) | BmsWarning | 0→非0 报 active；非0→0 报 recovered |

规则：

- 同一告警实例由 `sn + source + code` 关联；`state=0`（recovered）时 `code` 必须与对应 active 事件一致（设备端记住最后上报的码值）。
- 当多个源同时变化时，按 fault > warning 优先级逐条上报（最多 3 条）。
- level 变化（同 code 从 warning 变 fault 或反之）也作为新事件上报。
- 设备只发送稳定编码，名称、描述和多语言文本由后端字典映射。
- **位掩码拆分**：FaultValue / Warning / BmsWarning 均为位掩码（可能多位同时置 1），设备按「整体 0↔非 0」上报一条事件；服务端收到后须将位掩码按型号告警字典**逐位拆分**为具体告警条目（告警码 = 位索引），同一条 MQTT 事件可落库多条告警记录。

---

## 9. 配置上报 config

主题 `cs_inv/{sn}/config` · QoS 1 · **阶段 2 实现**（ESP32 需新增 READ_CTRLPARAM 0x01 读取控制参数 84B）。

触发：MQTT 连接成功、控制参数被写入后（CtrlParamAlterTime 变化）、收到 `query_config` 后。

```json
{
  "t": 1783676930,
  "v": 2,
  "data": {
    "rev": 1783676800,
    "params": {
      "output_priority": 0,
      "max_charge_current": 200,
      "battery_capacity": 100,
      "battery_type": 0,
      "output_voltage": 2200,
      "output_frequency": 5000,
      "soc_cutoff": 20,
      "...": "..."
    }
  }
}
```

| `data` 字段 | 类型 | 必填 | 定义 |
|------|------|------|------|
| `rev` | uint32 | 是 | 控制参数修改时间（CtrlParamAlterTime，Unix 秒），服务端据此判断配置是否更新 |
| `params` | object | 是 | 42 个控制参数的键值对象；键名与第 11.2 节命令清单同源，值为 ARM 原始 u16（含缩放）；未读取到/不可用的参数键可省略 |

服务端 `desired` 与 `reported` 分离：只有在设备上报新 `rev` 后才更新 reported，禁止用下发值覆盖实际值。

---

## 10. 网络状态 status / LWT

主题 `cs_inv/{sn}/status` · QoS 1 · Retain（现有 ESP32 实现，沿用不改）。

- **上线**（MQTT 连接成功）：立即发布一次

```json
{"t": 1783676930, "v": 2, "data": {"online": true, "rssi": -45, "ip": "192.168.1.100"}}
```

- **LWT 遗嘱**（broker 在异常断开时代发，Retain）：

```json
{"t": 0, "v": 2, "data": {"online": false}}
```

- 不做 60s 周期刷新（v1.0 稿为 60s，与 V1.0.26.713「Retain + LWT」模式不一致）；业务健康（work_state、故障、数据新鲜度）由 heartbeat 判断，与网络在线状态分开。
- 在线判定（服务端）：超过 420s 无有效上行数据即判定离线。

---

## 11. 控制命令

### 11.1 下行链路

```
管理后台 → POST /api/v1/devices/by-sn/{sn}/control {cmd, args, task_id}
    → API Server → device-communication（HTTP 内部调用）
    → MQTT 发布 cs_inv/{sn}/cmd（信封）
    → ESP32 解析 → UART WRITE_CTRLPARAM(0x02, 地址, 长度, 值) → ARM 执行
    → ARM 返回 → ESP32 发布 cs_inv/{sn}/cmd/response
    → device-communication → API Server 更新命令日志
```

### 11.2 下行信封（cs_inv/{sn}/cmd）

与 V1.0.26.713 下行命令信封一致：

```json
{
  "v": 2,
  "task_id": "019f5b9e-7bd7-7e50-a14d-5c8a74235a10",
  "t": 1783676930,
  "cmd": "set_max_charge_current",
  "args": [50]
}
```

| 字段 | 类型 | 必填 | 定义 |
|------|------|------|------|
| `v` | uint16 | 是 | 协议版本，固定 `2` |
| `task_id` | string | 是 | 命令唯一标识（服务端生成），幂等去重依据；命令日志按 task_id 记录生命周期 |
| `t` | int64 | 是 | 下发时间，Unix 秒 |
| `cmd` | string | 是 | 命令名（见 11.3 清单） |
| `args` | array | 是 | 参数数组；本协议命令均为单值命令，取 `args[0]`；查询类命令为 `[]` |

> **兼容性**：ESP32 解析器同时兼容旧格式 `{"cmd": "...", "value": <number>}`（及 `name`/`action`、`param` 别名），便于过渡期服务器双发；新服务器必须使用标准信封。

### 11.3 命令清单（命令 → 参数地址映射）

命令清单与 ESP32 固件 `cmd_handler.c` 现有映射表一致（53 项，启动日志 `CMD: init done (53 cmds)`），值为**工程单位**（浮点），ESP32 按 scale 写入 ARM 原始值：

| command_code | 地址 | 参数（工程单位） | scale | 说明 |
|--------------|------|------|------|------|
| `output_priority` | 0x0000 | priority:int | 1 | 输出优先级 |
| `set_charge_limit` | 0x0001 | current_a:float | 10 | 最大充电电流 0.1A（v1.0 稿写作 `set_max_charge_current`，与固件不符，本版统一为固件名） |
| `ac_volt_range` | 0x0002 | range:int | 1 | 交流输入电压范围 |
| `battery_capacity` | 0x0003 | capacity_ah:int | 1 | 电池容量 |
| `battery_type` | 0x0004 | battery_type:int | 1 | 电池类型 |
| `overload_restart` | 0x0005 | enable:int | 1 | 过载自动重启 |
| `high_temp_restart` | 0x0006 | enable:int | 1 | 高温自动重启 |
| `output_voltage` | 0x0007 | voltage_v:float | 10 | 输出电压 0.1V |
| `output_frequency` | 0x0008 | freq_hz:float | 100 | 输出频率 0.01Hz |
| `master_slave` | 0x0009 | mode:int | 1 | 主从模式 |
| `set_ac_charge_current` | 0x000A | current_a:float | 10 | 市电充电电流 0.1A |
| `low_volt_return_city` | 0x000B | voltage_v:float | 10 | 低压转市电 0.1V |
| `high_volt_return_bat` | 0x000C | voltage_v:float | 10 | 高压转电池 0.1V |
| `bat_charge_priority` | 0x000D | priority:int | 1 | 充电优先级 |
| `alarm_control` | 0x000E | alarm_ctrl:int | 1 | 告警控制 |
| `backlight_ctrl` | 0x000F | ctrl:int | 1 | 背光控制 |
| `power_shutdown_alarm` | 0x0010 | enable:int | 1 | 关机告警 |
| `overload_use_city_power` | 0x0011 | enable:int | 1 | 过载转市电 |
| `max_dischg_curr` | 0x0012 | current_a:float | 10 | 最大放电电流 0.1A |
| `max_chg_curr` | 0x0013 | current_a:float | 10 | 最大充电电流 0.1A |
| `recover_threshold_volt` | 0x0014 | voltage_v:float | 10 | 恢复阈值电压 0.1V |
| `solar_power_balance` | 0x0015 | enable:int | 1 | 光伏功率平衡 |
| `ac_output_mode` | 0x0016 | mode:int | 1 | AC 输出模式 |
| `li_bat_material` | 0x0017 | material:int | 1 | 锂电池材料 |
| `cell_serial_lifepo4` | 0x0018 | serial:int | 1 | LFP 串联数 |
| `cell_serial_li_nmc` | 0x0019 | serial:int | 1 | NMC 串联数 |
| `eq_enable` | 0x001A | enable:bool | 1 | 均衡使能 |
| `eq_voltage_set` | 0x001B | voltage_v:float | 10 | 均衡电压 0.1V |
| `eq_time_set` | 0x001C | time_min:int | 1 | 均衡时间 |
| `eq_timeout_set` | 0x001D | timeout:int | 1 | 均衡超时 |
| `eq_interval_set` | 0x001E | interval:int | 1 | 均衡间隔 |
| `eq_activate_set` | 0x001F | activate:int | 1 | 均衡激活 |
| `charge_time_set` | 0x0020 | time_min:int | 1 | 充电时间 |
| `close_charge_time_set` | 0x0021 | time_min:int | 1 | 关充电时间 |
| `lvolt_open_gen` | 0x0022 | voltage_v:float | 10 | 低压启动发电机 0.1V |
| `hvolt_close_gen` | 0x0023 | voltage_v:float | 10 | 高压关闭发电机 0.1V |
| `soc_back_utl` | 0x0024 | soc:int | 1 | 低电量转市电 % |
| `soc_back_bat` | 0x0025 | soc:int | 1 | 高电量转电池 % |
| `soc_back_gen` | 0x0026 | soc:int | 1 | 低电量启动发电机 % |
| `soc_close_gen` | 0x0027 | soc:int | 1 | 高电量关闭发电机 % |
| `set_soc_low` | 0x0028 | soc:int | 1 | 低电量截止 % |
| `buzzer_en` | 0x0029 | enable:bool | 1 | 蜂鸣器使能 |
| `s_grade_soc` | 0x002A | soc:int | 1 | S 级 SOC % |
| `a_grade_soc` | 0x002B | soc:int | 1 | A 级 SOC % |
| `b_grade_soc` | 0x002C | soc:int | 1 | B 级 SOC % |
| `manual_socket` | 0x002D | socket:int | 1 | 手动插座 |
| `control_socket` | 0x002E | socket:int | 1 | 控制插座 |
| `recover_threshold_soc` | 0x002F | soc:int | 1 | 恢复阈值 SOC % |
| `li_bat_protocol_type` | 0x0030 | type:int | 1 | 锂电协议类型 |
| `gen_rate_watt` | 0x0031 | watt:int | 1 | 发电机额定功率 W |
| `ge_balance_en` | 0x0032 | enable:bool | 1 | 发电机平衡使能 |
| `soc_max_utl_chg` | 0x0052 | soc:int | 1 | 市电充电截止 SOC % |
| `vmax_utl_chg` | 0x0053 | voltage_v:float | 10 | 市电充电截止电压 0.1V |
| `set_utc_time` | 0x00CA | utc:int(秒) | 1 | UTC 时间同步（写 32 位，差值>4s 校时）；**固件由 SNTP 模块自动执行（每日一次），无需云端下发** |

**查询命令**（args 为空数组，无参数）：

| command_code | 触发行为 |
|--------------|---------|
| `query_telemetry` | 立即补报一次 heartbeat（180s 周期重置） |
| `query_info` | 立即补报一次 info |
| `query_config` | 立即补报一次 config（阶段 2） |

### 11.4 命令响应（cs_inv/{sn}/cmd/response）

信封内 `data` 与现有固件 `cmd_handler.c` 输出一致，扩展 `task_id`：

```json
{
  "t": 1783676930,
  "v": 2,
  "data": {
    "task_id": "019f5b9e-7bd7-7e50-a14d-5c8a74235a10",
    "cmd": "set_max_charge_current",
    "result": "OK",
    "err": 0
  }
}
```

| 字段 | 类型 | 定义 |
|------|------|------|
| `task_id` | string | 回显下行 task_id；旧格式命令无 task_id 时省略 |
| `cmd` | string | 命令名 |
| `result` | string | `OK` / `INVALID_ARGS` / `NOT_SUPPORTED` / `BUSY` / `EXPIRED` / `EXEC_FAILED`（枚举沿用现有） |
| `err` | int | 0=成功；-1 执行失败；-2 参数错误；-3 忙；-4 不支持；-5 越界；-6 过期 |

> 命令在 2 秒内应答；ARM 写响应 `err!=0` 或 800ms 超时 → `EXEC_FAILED`（err=-1）。

---

## 12. 服务器解析链路

```
ESP32 --MQTT--> EMQX --webhook--> mqtt-kafka-bridge --Kafka(inv-telemetry)--> ProtocolParser
    → 按 topic + v 分派 → v==2 走 ParseHeartbeatV2（heartbeat 主题）
    → SaveTelemetryV2（device_telemetry_3min 新列）
    → Redis realtime:latest:{sn}（前端实时展示）
    → 状态机（在线/故障检测）
```

### 12.1 解析要点

- `ProtocolParser` 对 `heartbeat` 主题先读信封 `v` 字段：`v==1` 走 `ParseHeartbeat`（V1 数组），`v==2` 走 `ParseHeartbeatV2`（本协议 6.2 数组）；其余主题（info/alarm/config/status/cmd/response/ota）按 V1.0.26.713 通用解析。
- V2 解析器复用 V1 的校验模式：组长度精确匹配（sys=11/pv=5/ac=11/chr=3/bat=5/eng=14）、数值或 null 校验、范围 bounded、QualityFlags（NullValue/OutOfRange/ClockInvalid）。
- `work_state` 由 `sys_status` 按 6.3 推导写入 `device_telemetry_3min.work_state` 列（枚举与 V1 兼容）。
- `battery_power` 按 6.3 推导写入 `battery_power` 列（充电为正）。
- 新增 28 列写入 `device_telemetry_3min` 与 `device_latest_state`。
- Redis `realtime:latest:{sn}` 按组结构缓存（字段还原为工程单位）。

### 12.2 只读参数处理

只读参数（版本/SN/额定值）通过 `info` 主题上报（见第 7 节），服务器更新 `devices` 表（model_id 自动绑定 CS-L10-6K2）；心跳不含只读字段，避免重复传输。

---

## 13. 数据库型号配置

迁移脚本 `database/migrations/091_add_model_csl10_6k2.up.sql` 完成：

1. `telemetry_field_catalog` 新增 **28 个字段**（见第 14.4 节，`battery_overcharge` 仅 system 组一处）。
2. `device_protocol_versions` 新增 `heartbeat` v2（schema_hash `heartbeat-v2-csl10-6k2-20260802`）。
3. `device_protocol_fields` 按第 6.2 节顺序写入 **49 个位置**（wire_type 全部 float32，能量字段 float64）。
4. `device_models` 插入 `CS-L10-6K2`（辰烁科技 / inverter / 6.2kW）。
5. `device_model_fields` 配置显示：分组（系统/PV/AC/充电/电池/能量）、sort_order、show_realtime、show_history。
6. `device_model_commands` 写入第 11.3 节命令清单（parameter_schema 驱动前端表单，含查询命令）。
7. `device_telemetry_3min` + `device_latest_state` 新增 28 列。

---

## 14. 字段优化对照表

采集器协议 70 个运行参数 → V2 心跳 49 个位置值。

### 14.1 直接复用（20 个，映射到现有字段）

| 协议字段 | V2 位置 | 复用字段键 |
|----------|---------|-----------|
| Vpv1 | pv[0] | `pv1_voltage` |
| Vpv2 | pv[2] | `pv2_voltage` |
| Ppv | pv[4] | `pv_total_power` |
| OutputWatt | ac[2] | `output_power` |
| OutputVA | ac[3] | `output_apparent_power` |
| OutputCurr | ac[4] | `output_current` |
| ACOutputVolt | ac[0] | `ac_output_voltage` |
| ACOutputFreq | ac[1] | `ac_output_frequency` |
| BatVolt | bat[0] | `battery_voltage` |
| BatterySOC | bat[1] | `battery_soc` |
| BatChgCurr / BatDischgCurr | bat[2] | `battery_current`（符号合并） |
| BusVolt | sys[8] | `dc_bus_voltage` |
| LoadPercent | sys[9] | `load_percent` |
| InvertTemp | sys[4] | `inverter_temperature` |
| Epv_today / Epv_total | eng[2] / eng[3] | `pv_energy_daily` / `pv_energy_total` |
| Ebat_dischrToday / Total | eng[6] / eng[7] | `battery_discharge_energy_daily` / `_total` |
| Ebat_chrToday / Total | eng[8] / eng[9] | `battery_charge_energy_daily` / `_total` |

### 14.2 服务器推导（2 个，不上报）

| 推导字段 | 公式 | 说明 |
|----------|------|------|
| `work_state` | sys_status 位组合（见 6.3） | 枚举与 V1 完全一致 |
| `battery_power` | Charge→+bat[3]；Discharge→-bat[4] | 充电为正 |

### 14.3 剔除 / 合并

| 处理 | 协议字段 | 原因 |
|------|----------|------|
| 剔除 | cells（电芯电压/温度） | 采集器协议无电芯数据 |
| 剔除 | mos_temperature / ambient_temperature / fan_speed / runtime_hours | 协议无对应地址 |
| 剔除 | battery_soh / cycle_cnt / 容量 / 单体电压 / BMS 电流电压请求 | BMS 通信字段，协议无数据源 |
| 合并 | BatChgCurr + BatDischgCurr → battery_current（带符号） | 同一母线电流 |
| 合并 | BatOverCharge → sys[10] | 去重（v1.0 稿 bat[5] 冗余已删除） |
| 保留 | Ppv → pv[4] 直接上报 | 协议已给总功率，不拆分 |

### 14.4 新增字段目录（28 个，迁移写入 telemetry_field_catalog）

| 分组 | 字段键 |
|------|--------|
| system（7） | `sys_status`、`warning`、`bms_warning`、`boost_temperature`、`transformer_temperature`、`pv_temperature`、`battery_overcharge` |
| pv（2） | `buck1_current`、`buck2_current` |
| ac（9） | `grid_voltage`、`grid_frequency`、`ac_input_power`、`ac_input_apparent_power`、`ac_charge_power`、`ac_charge_apparent_power`、`ac_charge_current`、`ac_discharge_power`、`ac_discharge_apparent_power` |
| battery（2） | `battery_charge_power`、`battery_discharge_power` |
| energy（8） | `gen_energy_daily`、`gen_energy_total`、`ac_charge_energy_daily`、`ac_charge_energy_total`、`ac_discharge_energy_daily`、`ac_discharge_energy_total`、`output_energy_daily`、`output_energy_total` |

> 7 + 2 + 9 + 2 + 8 = **28 个唯一键**（v1.0 稿误写 26/27，ac 组漏数 1 个）。

---

## 15. 实施清单与验证

| # | 项 | 文件 | 状态 |
|---|----|------|------|
| 1 | 协议设计文档（本修订版） | `docs/CS-L10-6K2_MQTT_上报协议设计_V2.0.md` | 本文档 |
| 2 | 数据库迁移 | `database/migrations/091_add_model_csl10_6k2.up.sql` / `.down.sql` | 待执行 |
| 3 | V2 解析器 | `device-communication/internal/telemetry/heartbeat_v2.go` | 待实施 |
| 4 | Sample 扩展 | `device-communication/internal/telemetry/model.go` | 待实施 |
| 5 | 解析分支 | `device-communication/internal/service/protocol_parser.go` | 待实施 |
| 6 | 落库扩展 | `device-communication/internal/repository/telemetry_repository.go` | 待实施 |
| 7 | 单元测试 | `device-communication/internal/telemetry/heartbeat_v2_test.go` | 待实施 |
| 8 | **ESP32 固件：heartbeat 组包（49 数组 + 信封 v=2）** | `esp32c3_l10_idf/main/telemetry/telemetry.c` | 待实施 |
| 9 | **ESP32 固件：info 新格式** | `esp32c3_l10_idf/main/telemetry/telemetry.c` | 待实施 |
| 10 | **ESP32 固件：alarm 新格式（source/code/level/state）** | `esp32c3_l10_idf/main/telemetry/telemetry.c` | 待实施 |
| 11 | **ESP32 固件：查询命令 query_telemetry/query_info** | `esp32c3_l10_idf/main/telemetry/cmd_handler.c` | 待实施 |
| 12 | **ESP32 固件：config 上报（阶段 2，含 READ_CTRLPARAM）** | `esp32c3_l10_idf/main/telemetry/` | 待实施 |
| 13 | 控制面板 | `inv-admin-frontend/src/pages/remote-settings/` | 待实施 |
| 14 | 实时展示 | `inv-admin-frontend/src/pages/device-detail/StatusTab.tsx` | 待实施 |
| 15 | 能量流图 | `inv-admin-frontend/src/pages/stations/components/EnergyFlowDiagram.tsx` | 待实施 |

验证命令：

```bash
make build-device && go test ./device-communication/internal/telemetry/... && make vet-go
# 迁移脚本测试库执行：psql -f 091_add_model_csl10_6k2.up.sql
# 前端：make type-check
# ESP32：idf.py build（构建后接模拟器验证：heartbeat 主题每 180s、信封 v=2、数组长度 49）
```

---

## 附录 A：与原始设计稿修订对照

| # | 原始设计稿（v1.0） | 问题 | 本版修订 |
|---|-------------------|------|---------|
| 1 | 主题 `cs_inv/{sn}/data/telemetry` | 服务器已有 heartbeat 解析路径，换主题需额外路由 | 统一 `cs_inv/{sn}/heartbeat`，按 `v` 分派 |
| 2 | 频率 5s | 与 V1.0.26.713（180s）冲突 | 180s + 启动立即 + query_telemetry 补报 |
| 3 | QoS 0 | 与 V1.0.26.713（QoS 1）冲突，离线补传失效 | QoS 1 |
| 4 | work_state 推导 {Fault→1, Charge→2, Discharging→3, ACBypass→4} | 与 V1.0.26.713 枚举（0待机/1逆变/2旁路/3关机/4故障）冲突 | 统一 V1.0.26.713 枚举（Fault→4、ACBypass→2、StandBy→0、其他→1） |
| 5 | battery_power 放电为正 | 与 V1.0.26.713（充电为正）相反 | 充电为正 |
| 6 | info 平铺无信封、含 `sn`、键名 `type`/`rated_freq`/`battery_voltage` | 违反信封规范、SN 重复、键名不一致 | 信封 + 去 sn + 键名统一（device_type/rated_frequency/battery_nominal_voltage）+ 补必填字段 |
| 7 | 1.1 原则"Ppv 不上报" | 与 pv[4]=pv_total_power、5.3 矛盾 | 原则改为"ARM 已给出总量的字段不拆分" |
| 8 | bat[5] battery_overcharge | 与 sys[10] 冗余 | 删除，bat 数组 5 个 |
| 9 | battery_soc 单位 0.1% | ARM 仅有整数 % | 1% |
| 10 | eng 范围 0-1e12 | 超 ARM u32 上限 | 0-4.29e9 |
| 11 | 新增字段数 26/27 自相矛盾 | ac 组实为 9 个 | 28 个 |
| 12 | "直接复用 25 个" | 实际列表 20 个 | 20 个 |
| 13 | 控制参数 "~60" | 实为 42 个 u16（84B） | 84B / 42 个 u16 |
| 14 | `ac_bypass_power` / `ac_bypass_apparent_power` | 语义不准（实为交流放电） | `ac_discharge_power` / `ac_discharge_apparent_power` |
| 15 | status 60s 周期 | 与 V1.0.26.713（Retain+LWT）不一致 | 上线一次 + LWT，不周期刷 |
| 16 | 无 alarm 格式定义 | 仅写"沿用" | 定义 {source, code, level, state} + L10 告警源映射（8.1） |
| 17 | 无 config 主题 | V1.0.26.713 必备 | 新增第 9 节（阶段 2） |
| 18 | 无查询命令 | V1.0.26.713 要求 | 新增 query_telemetry / query_info / query_config |
| 19 | 命令无 task_id、信封未定义 | 与 V1.0.26.713 下行信封不一致 | 定义下行信封 {v, task_id, t, cmd, args} + 兼容旧 {cmd, value}；cmd/response 扩展 task_id |
| 20 | 伪代码 soc/温度示例与表矛盾 | — | 示例与字段定义对齐 |
| 21 | 命令清单 52 项、0x0001 命名 `set_max_charge_current` | 固件 `s_cmd_map` 实为 53 项，0x0001 实为 `set_charge_limit` | 53 项；0x0001 统一为 `set_charge_limit`；`set_utc_time` 标注为 SNTP 自动执行、非云端命令 |
| 22 | bms_warning 范围 0-2^32、功率字段数据源同值未注明 | ARM `BmsWarning` 为 u16；`ACInWatt/ACDisChrWatt/BatDisChrWatt` 均与 `ACChrWatt/BatChrWatt` 同值（Pbat） | 范围 0-65535；6.2 补充数据源同值注记（充放语义按 sys_status 位/电流符号区分） |
