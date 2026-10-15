# CS-L10-6K2 逆变器 ESP32→云端 MQTT 上报协议设计 V2.1

> **文档版本**: v2.1
> **适用产品**: CS-L10-6K2 48V 单相离网逆变器（GD32F30x 系统）
> **适用对象**: ESP32 固件开发（esp32c3_l10_idf）、云端（device-communication / business-api / 管理后台 / App）开发
> **最后更新**: 2026-08-05
> **上游协议**: `collector_protocol_print.html`（辰烁采集器通信协议规范，ARM↔ESP32 UART）
> **协议基线**: `辰烁科技48V离网逆变器遥测协议与存储重设计V1.0.26.713.html`（全系统 MQTT 协议基线，本协议为其在 CS-L10-6K2 型号上的落地实现）
> **上一版本**: `CS-L10-6K2_MQTT_上报协议设计_V2.0.md`（本版据此修订，修订对照见第 16 节）

---

## 目录

1. [概述](#1-概述)
2. [与既有协议的关系](#2-与既有协议的关系)
3. [采集器协议摘要（UART 层）](#3-采集器协议摘要uart-层)
4. [主题与上报频率](#4-主题与上报频率)
5. [上行信封](#5-上行信封)
6. [V2.1 心跳定义（57 值）](#6-v21-心跳定义57-值)
7. [设备信息 info（17 字段业务化）](#7-设备信息-info17-字段业务化)
8. [告警事件 alarm](#8-告警事件-alarm)
9. [配置上报 config（v2 语义键值对）](#9-配置上报-configv2-语义键值对)
10. [网络状态 status / LWT](#10-网络状态-status--lwt)
11. [控制命令与命令闭环](#11-控制命令与命令闭环)
12. [服务器解析链路](#12-服务器解析链路)
13. [数据库型号配置（迁移 096）](#13-数据库型号配置迁移-096)
14. [利用层规则：诊断与健康度](#14-利用层规则诊断与健康度)
15. [ESP32 固件规范](#15-esp32-固件规范)
16. [与 V2.0 修订对照](#16-与-v20-修订对照)
17. [实施清单与验证](#17-实施清单与验证)

---

## 1. 概述

CS-L10-6K2 是辰烁科技 48V 单相离网逆变器，ARM 主控（GD32F30x）通过 USART 与 ESP32 采集器通信（PPP 帧 + TEA 双重加密 + CRC32，地址映射式读写）。ESP32 负责将 ARM 数据经 MQTT 上报云端，并转发云端控制命令。

V2.1 在 V2.0 基础上的三个核心变化：

1. **心跳 49 → 57 值**：V2.0 因"采集器协议无数据源"剔除的字段（风扇转速、逆变电流、并机电流、累计运行时长、插座状态）在 V2.1 **全部回归**；`sys[6]/sys[7]`（变压器/PV 温度）位置保留，ARM 赋值后如实上报，未赋值填 `null`。原有 49 个位置**一个不动**，新增 8 个位置全部附加在组尾，纯 additive 演进。
2. **info 业务化**：17 个只读字段除落库外，逐一映射到业务逻辑（负载率计算、OTA 可用性、型号能力绑定、电池策略提示），并在管理后台/App 提供对应展示（见第 7.2 节映射表）。
3. **config 重设计**：弃用"42 个 u16 平铺数组"，改为**语义键值对**（键名与命令清单同源，值为工程单位），服务端按 `device_config_schema` 校验，`desired/reported` 闭环，配置漂移自动诊断。

### 1.1 设计原则

| 原则 | 说明 |
|------|------|
| additive | V2.0 的 49 个位置冻结不动，新增组/位置只追加；旧固件（49 值）与服务端新解析器兼容（长度校验按版本分派） |
| 业务化 | 每个上报参数都要"有用"：要么驱动业务逻辑（诊断/OTA/负载率），要么支撑前端展示；不允许只落库不利用 |
| 位置数组 | 心跳沿用组结构（group_code + field_index），数组长度、下标、类型和单位永久冻结 |
| 原始量纲 | 心跳上报 ARM 协议原始整数（含 0.1 缩放），服务器按字段定义还原，避免浮点误差 |
| 工程单位 | config 上报工程单位（服务器侧命令语义键），由 ESP32 固件负责 ARM 原始值 ↔ 工程单位换算 |
| 元数据驱动 | 字段定义、显示配置、控制命令、config schema 全部走数据库配置，不改代码 |
| 单一快照 | 整机实时数据统一由 heartbeat 上报（180s），不拆分多主题 |
| DB 权威 | 展示层以数据库为准（upsert 已落库），Redis realtime 仅作即时缓存 |

### 1.2 与 V1.0.26.713 / V2.0 的字段取舍演进

| 阶段 | 处理 | 说明 |
|------|------|------|
| V2.0 | 剔除 `fan_speed` / `runtime_hours` 等 | 当时采集器协议未确认数据源 |
| V2.1 | **全部回归**：`mppt_fan_speed`/`inv_fan_speed`（%）、`inv_current`（0.1A）、`parallel_charge_current`（1A）、`work_time_total`（s）、`paired_socket`/`online_socket`/`on_socket`（位掩码） | 固件确认 ARM 已提供对应地址，见 6.2.7-6.2.9 |
| V2.1 | `sys[6]`/`sys[7]` 如实上报 | ARM 未赋值时填 `null`，服务端容忍 |

---

## 2. 与既有协议的关系

| 文档 | 角色 |
|------|------|
| `辰烁科技48V离网逆变器遥测协议与存储重设计V1.0.26.713.html` | **全系统协议基线**：信封 `{t,v,data}`、主题命名、QoS、存储设计、命令信封、告警字典规则均以其为准 |
| `CS-L10-6K2_MQTT_上报协议设计_V2.0.md` | 上一版型号协议（已并入本版，修订对照见第 16 节） |
| 本文档 v2.1 | **CS-L10-6K2 型号协议现行版**：定义 L10 心跳 schema（v=2，57 值）、info 17 字段业务语义、config v2 语义键值对 |

- L10 设备上行 **heartbeat 主题与 V1 相同**（`cs_inv/{sn}/heartbeat`），信封 `v=2` 不变（**不升 v=3**），通过 `schema_hash` 区分组包版本；服务器 V2 解析器按组长度自适应 49/57（`sys[11]/pv[5]/ac[11]/chr[3]/bat[5]/eng[14]/fan[2]/diag[3]/sock[3]`）。
- 49 值旧固件与 57 值新固件**可以混跑**：旧固件不携带 fan/diag/sock 组，服务器标记 QualityPartial，其余字段照常解析。
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
| 0x01 | READ_CTRLPARAM | 读控制参数（0x0000-0x0053，84B / 42 个 u16） |
| 0x02 | WRITE_CTRLPARAM | 写控制参数 |
| 0x03 | READ_RUNPARAM | 读运行参数（0x03E8-0x0442，176B） |
| 0x04 / 0x05 | READ/WRITE_R_W_DATA | 读写可读写数据（0x00C8-0x00CB：CtrlParamAlterTime、UTC） |
| 0x06 | READ_R_DATA | 读只读数据（0x0190-0x01A9：版本/SN/额定值，52B） |
| 0x07 | CTRLCMD | 控制命令（预留） |
| 0x14 | READ_TEST | 读测试参数（单位与运行参数不同） |

### 3.3 参数地址空间

| 区域 | 地址 | 数量 | 说明 |
|------|------|------|------|
| 控制参数 | 0x0000-0x0053 | 84 字节 / 42 个 u16 | 输出优先级、充电电流、电池容量/类型、电压频率、主从、SOC 阈值、均衡、发电机、时间等（参数序号 0x0000-0x0029，见 9.2） |
| 可读写 | 0x00C8-0x00CB | 4 字节 | CtrlParamAlterTime（u32）、UTC 时间（u32，写入自动校时） |
| 运行参数 | 0x03E8-0x0442 | 176 字节 / 88 个 u16 | 见第 6.2 节字段定义 |
| 只读 | 0x0190-0x01A9 | 52 字节 / 26 个 u16 | ARM/DSP 版本、模块号、SN（4×u32，16B ASCII）、硬件版本、额定参数、Bootloader 版本（见 7.1） |

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
| `cs_inv/{sn}/heartbeat` | 1 | false | **180s** + 启动立即 + `query_telemetry` 补报 | ARM | **V2.1 心跳（57 值，本协议核心）** |
| `cs_inv/{sn}/info` | 1 | false | 连接时、信息变化、`query_info` 后 | ARM 只读 | 设备能力与固件信息（17 字段） |
| `cs_inv/{sn}/alarm` | 1 | false | 发生、级别变化、恢复（不等待心跳） | ARM | 告警事件闭环 |
| `cs_inv/{sn}/config` | 1 | false | 连接时、配置生效后、`query_config` 后 | ARM 控制参数 | 设备实际生效配置（v2 语义键值对） |
| `cs_inv/{sn}/status` | 1 | true | 上线一次 + LWT | ESP32 自动 | 网络在线状态（online + rssi + ip），不周期刷 |
| `cs_inv/{sn}/cmd` | 1 | — | 按需（云端→ESP32） | 云端 | 控制命令下行 |
| `cs_inv/{sn}/cmd/response` | 1 | false | 按需（ESP32→云端） | ESP32 | 命令执行结果上行 |
| `cs_inv/{sn}/ota/cmd`、`ota/status` | 1 | — | 按需 | — | OTA（沿用现有） |
| `cs_inv/{sn}/debug/log` | 0 | false | 仅调试模式生效期间，按批（≤1 批/s） | ESP32 | 串口日志上云，便于远程判读（ARM 轮询超时、OTA、WiFi 等）；后端不解析 |

> **调试模式（`set_debug_telemetry`）**：设备侧把 heartbeat 周期从 180s 临时切到 `args=[enabled, interval_s, duration_s]` 指定值（默认 5s，5..3600s；时长默认 1h，上限 24h），到期、收到关闭命令或重启后自动恢复 180s，不持久化。生效期间串口日志同时按批上行到 `cs_inv/{sn}/debug/log`，payload 为 `{"lines":["…"],"n":<行数>[,"dropped":<溢出丢弃字节数>]}`（`lines` 为串口日志原文，不含时间戳；信封 `t` 为上报时刻）。关闭调试模式后不再上行。

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
- 实时遥测使用**原始量纲**（含 0.1 缩放，见 6.2 单位列）；config 使用**工程单位**（见第 9 节）。
- SN 只从 Topic 获取，不在 `data` 中重复发送。
- V2 不使用全局消息序号；重复投递由服务端按 `sn + topic + t + data_hash` 在短窗口内消除（`data_hash` 由服务端计算，设备不发送）。

---

## 6. V2.1 心跳定义（57 值）

### 6.1 总览

```json
{
  "v": 2,
  "t": 1783000000,
  "data": {
    "sys":  [11],
    "pv":   [5],
    "ac":   [11],
    "chr":  [3],
    "bat":  [5],
    "eng":  [14],
    "fan":  [2],
    "diag": [3],
    "sock": [3]
  }
}
```

- 各分组为**定长位置数组**，元素必须是数值或 `null`（`null` 表示该字段本次无数据，服务器标记 QualityPartial）。
- 上报值为 ARM 协议原始整数（已含缩放），**负数直接使用 JSON 负数**（如电池放电电流 -255 = -25.5A）。
- 与 V2.0 相比：`sys/pv/ac/chr/bat/eng` 六组**位置与语义完全不变**（仅 `bat` 保持 5 值、`battery_soc` 为 1%），新增 `fan[2]/diag[3]/sock[3]` 三组共 **8 个位置**。
- **49 值旧固件兼容**：旧固件可不发 `fan/diag/sock` 组（或发 `null` 组），服务器按长度自适应。

### 6.2 组字段顺序定义（服务器解析依据）

#### 6.2.1 sys（11 个）— 系统状态（与 V2.0 完全一致）

| 索引 | 字段键 | 说明 | 单位 | 范围 | ARM 来源 |
|------|--------|------|------|------|---------|
| 0 | `sys_status` | 系统状态位（12 位 bitmask，见 3.4） | - | 0-4095 | SysStatus |
| 1 | `fault_code` | 故障码 | - | 0-2^32 | FaultValue |
| 2 | `warning` | 告警位（64 位） | - | 0-2^64 | Warning |
| 3 | `bms_warning` | BMS 告警 | - | 0-65535 | BmsWarning（ARM 为 u16） |
| 4 | `inverter_temperature` | 逆变温度 | 0.1°C | -40-100 | InvertTemp |
| 5 | `boost_temperature` | Boost 温度 | 0.1°C | -40-120 | BoostTemp |
| 6 | `transformer_temperature` | 变压器温度 | 0.1°C | -40-120 | TransformerTemp（ARM 未赋值 → null，**V2.1 要求如实上报**） |
| 7 | `pv_temperature` | PV 温度 | 0.1°C | -40-120 | PvTemp（ARM 未赋值 → null，**V2.1 要求如实上报**） |
| 8 | `dc_bus_voltage` | 母线电压 | 0.01V | 0-500 | BusVolt |
| 9 | `load_percent` | 负载百分比 | 0.1% | 0-120 | LoadPercent |
| 10 | `battery_overcharge` | 过充标志 | - | 0-1 | BatOverCharge |

#### 6.2.2 pv（5 个）— 光伏（与 V2.0 完全一致）

| 索引 | 字段键 | 说明 | 单位 | 范围 | ARM 来源 |
|------|--------|------|------|------|---------|
| 0 | `pv1_voltage` | PV1 电压 | 0.1V | 0-150 | Vpv1 |
| 1 | `buck1_current` | Buck1 电流 | 0.1A | 0-30 | Buck1Curr |
| 2 | `pv2_voltage` | PV2 电压 | 0.1V | 0-150 | Vpv2 |
| 3 | `buck2_current` | Buck2 电流 | 0.1A | 0-30 | Buck2Curr |
| 4 | `pv_total_power` | PV 总功率（ARM 直接给出，不再拆分） | 0.1W | 0-7500 | Ppv |

#### 6.2.3 ac（11 个）— 交流

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
| 9 | `ac_bypass_power` | AC 放电功率 | 0.1W | 0-7500 | ACDisChrWatt |
| 10 | `ac_bypass_apparent_power` | AC 放电视在功率 | 0.1VA | 0-7500 | ACDisChrVA |

> **键名勘误（相对 V2.0）**：V2.0 文档将 ac[0]-ac[4] 定义为 `ac_output_voltage`/`ac_output_frequency`/`output_power`/`output_apparent_power`/`output_current`，但迁移 091 误用 V1 通用键名（`ac_voltage` 等）。**本版以文档键名为准**，迁移 096 修正 `device_protocol_fields` 与 `telemetry_field_catalog`。
> **键名勘误（相对 V2.0）**：V2.0 将 ac[9]/ac[10] 命名为 `ac_discharge_*`，迁移 091 实际使用 `ac_bypass_*` 且落库列/代码/前端字段配置均已按 `ac_bypass_*` 实现。**本版键名定为 `ac_bypass_power`/`ac_bypass_apparent_power`**（语义：交流放电功率，数据源 ACDisChrWatt），不再更名。
> **数据源同值注记**：ARM 上 `ACInWatt/ACDisChrWatt` 与 `ACChrWatt` 同值（均取 Pbat），充电/放电/输入语义需结合 `sys_status` 位（bit2 Charge / bit3 Discharging）区分，服务端不可单独用 ac[7]/ac[9] 判断充放方向。

#### 6.2.4 chr（3 个）— AC 充电（与 V2.0 完全一致）

| 索引 | 字段键 | 说明 | 单位 | 范围 | ARM 来源 |
|------|--------|------|------|------|---------|
| 0 | `ac_charge_power` | AC 充电功率 | 0.1W | 0-7500 | ACChrWatt |
| 1 | `ac_charge_apparent_power` | AC 充电视在功率 | 0.1VA | 0-7500 | ACChrVA |
| 2 | `ac_charge_current` | AC 充电电流 | 0.1A | 0-150 | ACChrCurr |

#### 6.2.5 bat（5 个）— 电池

| 索引 | 字段键 | 说明 | 单位 | 范围 | ARM 来源 |
|------|--------|------|------|------|---------|
| 0 | `battery_voltage` | 电池电压 | 0.01V | 0-70 | BatVolt |
| 1 | `battery_soc` | 电池 SOC | **1%** | 0-100 | BatterySOC（ARM 为整数 %） |
| 2 | `battery_current` | 电池电流（**充电为正，放电为负**） | 0.1A | -150-150 | SysStatus 位 2/3 选择 BatChgCurr / -BatDischgCurr |
| 3 | `battery_charge_power` | 电池充电功率 | 0.1W | 0-7500 | BatChrWatt |
| 4 | `battery_discharge_power` | 电池放电功率 | 0.1W | 0-7500 | BatDisChrWatt |

> `battery_soc` 为 **1%**（迁移 096 修正 091 的 0.1 scale 错误）。
> **数据源同值注记**：ARM 上 `BatDisChrWatt` 与 `BatChrWatt` 同值（均为 Pbat），充/放功率语义须由 `sys_status` 位或 `bat[2]` 电流符号决定。
> `battery_power`（服务端派生列，**充电为正**）：SysStatus 位 2（Charge）→ `+bat[3]`；位 3（Discharge）→ `-bat[4]`。

#### 6.2.6 eng（14 个）— 能量统计（键名以本表为准，迁移 096 修正 091 偏差）

| 索引 | 字段键 | 说明 | 单位 | 范围 | ARM 来源 |
|------|--------|------|------|------|---------|
| 0 | `gen_energy_daily` | 发电机今日发电 | 0.1kWh | 0-4.29e9 | EGen_today |
| 1 | `gen_energy_total` | 发电机总发电 | 0.1kWh | 0-4.29e9 | EGen_total |
| 2 | `daily_pv_energy` | PV 今日发电 | 0.1kWh | 0-4.29e9 | Epv_today |
| 3 | `total_pv_energy` | PV 总发电 | 0.1kWh | 0-4.29e9 | Epv_total |
| 4 | `ac_charge_energy_daily` | AC 今日充电 | 0.1kWh | 0-4.29e9 | Eac_chrToday |
| 5 | `ac_charge_energy_total` | AC 总充电 | 0.1kWh | 0-4.29e9 | Eac_chrTotal |
| 6 | `daily_discharge_energy` | 电池今日放电 | 0.1kWh | 0-4.29e9 | Ebat_dischrToday |
| 7 | `total_discharge_energy` | 电池总放电 | 0.1kWh | 0-4.29e9 | Ebat_dischrTotal |
| 8 | `daily_charge_energy` | 电池今日充电 | 0.1kWh | 0-4.29e9 | Ebat_chrToday |
| 9 | `total_charge_energy` | 电池总充电 | 0.1kWh | 0-4.29e9 | Ebat_chrTotal |
| 10 | `ac_bypass_energy_daily` | AC 今日放电 | 0.1kWh | 0-4.29e9 | Eac_dischrToday |
| 11 | `ac_bypass_energy_total` | AC 总放电 | 0.1kWh | 0-4.29e9 | Eac_dischrTotal |
| 12 | `output_energy_daily` | 输出今日放电 | 0.1kWh | 0-4.29e9 | Eop_dischrToday |
| 13 | `output_energy_total` | 输出总放电 | 0.1kWh | 0-4.29e9 | Eop_dischrTotal |

> **键名勘误（相对 V2.0）**：V2.0 文档 eng[2]/eng[3]/eng[6]-eng[9] 写作 `pv_energy_daily`/`battery_discharge_energy_*` 等，与 `telemetry_field_catalog` 既有 V1 字段（`daily_pv_energy`/`total_discharge_energy` 等）重复冲突。**本版以 catalog 既有键名为准**（避免 catalog 重复键），迁移 096 将 `device_protocol_fields` 键名统一到本表。

#### 6.2.7 fan（2 个）— 风扇转速（V2.1 新增）

| 索引 | 字段键 | 说明 | 单位 | 范围 | ARM 来源 |
|------|--------|------|------|------|---------|
| 0 | `mppt_fan_speed` | MPPT 风扇转速 | % | 0-100 | MpptFanSpeed |
| 1 | `inv_fan_speed` | 逆变风扇转速 | % | 0-100 | InvFanSpeed |

> 数值为风扇当前转速百分比（0=停转，100=全速）。**业务用途**：散热诊断输入（与温度组合判定风扇异常，见 14.1）、健康度扣分项（见 14.4）。

#### 6.2.8 diag（3 个）— 诊断量（V2.1 新增）

| 索引 | 字段键 | 说明 | 单位 | 范围 | ARM 来源 |
|------|--------|------|------|------|---------|
| 0 | `inv_current` | 逆变器输出电流 | 0.1A | 0-100 | InvCurrent |
| 1 | `parallel_charge_current` | 并机充电电流 | 1A | 0-600 | ParallelChgCurr |
| 2 | `work_time_total` | 累计运行时长 | s | 0-2^32 | WorkTimeTotal |

> `work_time_total` 为 **u32 秒**（迁移 096 中 wire_type=float64 以容纳大数），跨过 5000h（18,000,000s）触发维护提醒（见 14.3）。**业务用途**：并机充电能力判断（并联主机的充电电流上限）、维护周期管理。

#### 6.2.9 sock（3 个）— 插座状态（V2.1 新增）

| 索引 | 字段键 | 说明 | 单位 | 范围 | ARM 来源 |
|------|--------|------|------|------|---------|
| 0 | `paired_socket` | 已配对插座位掩码（bit0=插座1） | - | 0-2^16 | PairedSocket |
| 1 | `online_socket` | 在线插座位掩码 | - | 0-2^16 | OnlineSocket |
| 2 | `on_socket` | 运行中插座位掩码 | - | 0-2^16 | OnSocket |

> u16 位掩码，每 bit 对应一个并联从机插座。**业务用途**：并机拓扑诊断（paired>0 且 online<paired → 从机离线，见 14.2）、健康度扣分项。

### 6.3 派生字段（服务端计算，设备不上报）

| 派生字段 | 公式 | 说明 |
|----------|------|------|
| `work_state` | `sys_status` 位组合：Fault(bit1)→4、ACBypass(bit7)→2、StandBy(bit0)→0、其他→1 | 枚举与 V1.0.26.713 完全一致：0 待机 / 1 逆变 / 2 旁路 / 3 关机 / 4 故障；L10 无关机指示位，3 保留定义不使用 |
| `battery_power` | Charge(bit2)→`+bat[3]`；Discharge(bit3)→`-bat[4]` | 充电为正，放电为负（与 V1.0.26.713 `bat[4]` 语义一致） |
| `power_factor` | `ac[2] / ac[3]`（当 ac[3]>0） | 展示用，可选 |
| `parallel_role` | `paired_socket>0` 且 `online_socket≥1` → master；仅自身在线 → standalone | 并机角色判定，写入 realtime 与详情派生（见 12.4） |
| `parallel_health` | `paired>0 且 online<paired` → degraded；`online≥paired` → ok；无并机 → n/a | 并机健康状态（与 14.2 诊断联动） |
| `thermal_status` | 温度/风扇组合（见 14.1） | 散热状态：normal / warning / fault |

### 6.4 完整示例（57 值）

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
    "eng": [0, 0, 1250, 45678, 0, 0, 1305, 23456, 0, 0, 0, 0, 1870, 98765],
    "fan": [88, 76],
    "diag": [880, 45, 68400000],
    "sock": [3, 2, 2]
  }
}
```

> 数值均为协议原始量纲（已含缩放），服务器按字段定义还原（如 `ac[0]=2205` → 220.5V、`sys[8]=41000` → 410.00V、`bat[2]=-255` → -25.5A、`diag[0]=880` → 88.0A、`diag[2]=68400000` → 19000h）。
> 上例：2 台从机已配对、2 台在线、2 台运行中（位掩码 0b11/0b10/0b10）。

### 6.5 长度自适应规则

| 固件版本 | 组结构 | 服务器处理 |
|----------|--------|-----------|
| 49 值（V2.0 固件） | 六组，无 fan/diag/sock | 正常解析六组，fan/diag/sock 置空，QualityPartial |
| 57 值（V2.1 固件） | 九组完整 | 全部解析 |

- 服务器解析器按**组是否存在**自适应：`fan/diag/sock` 缺失或长度为 0 视为旧固件；存在但长度 ≠ 2/3/3 视为格式错误（isolate 到 ingest error）。
- 六组长度仍精确校验（sys=11/pv=5/ac=11/chr=3/bat=5/eng=14）。

---

## 7. 设备信息 info（17 字段业务化）

主题 `cs_inv/{sn}/info` · QoS 1 · 设备上线（MQTT 连接成功且已有 ARM 只读数据）、任一能力字段变化、收到 `query_info` 时上报。数据源：ARM 只读区 **0x0190-0x01A9（52B / 26 个 u16）** 经 0x06 READ_R_DATA 读取。

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

### 7.1 字段定义与 ARM 来源

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
| `rated_power` | uint32 | 是 | **W** | RateWatt（协议原值单位 W，服务器落 `rated_power_w` 列，展示层 ÷1000 换算 kW） |
| `rated_voltage` | number | 是 | V | NomOpVolt / 10 |
| `rated_frequency` | number | 是 | Hz | NomOpFreq / 100 |
| `battery_nominal_voltage` | number | 是 | V | NomBatVolt / 100（16S LFP 为 51.2） |
| `battery_type` | string | 是 | `LiFePO4` / `NCM` / `LeadAcid` / `Other` | 当前配置电池类型，由控制参数 0x0004 映射：0→LiFePO4、1→NCM、2→LeadAcid、其他→Other；未读取前按出厂默认 LiFePO4 |
| `cell_count` | uint16 | 是 | 节 | L10 无电芯数据，固定 `0` |
| `temp_sensor_count` | uint16 | 是 | 个 | 固定 `0` |
| `inverter_module` | string | 是 | — | 模块号（InverterModuleH/L 合并，BCD 转字符串，出厂追溯） |
| `hardware_version` | string | 是 | — | HardwareVersion 格式化（告警规则分流依据） |
| `bootloader_version` | string | 是 | — | BLVersion 格式化（OTA 引导兼容性判断） |

### 7.2 字段语义 → 业务利用 → 前端展示映射（核心）

服务器对每个只读字段做三件事：**落库 → 映射业务逻辑 → 驱动前端展示**。下表为 17 字段逐一说明"代表什么、服务器如何用、前端如何展示"。

| info 字段 | 业务语义 | 服务器利用 | 前端功能 |
|---|---|---|---|
| `model` | 型号编码 | 绑定 `device_models.model_id` → 决定协议解析器（v1/v2）、命令清单、字段元数据、OTA 目标；未知型号降级（保留字符串、model_id 置空） | 型号展示 + "未注册型号"提示（管理后台设备信息 Tab） |
| `manufacturer` | 制造商 | 落库 `devices.manufacturer` | 展示 |
| `firmware_arm` | ARM 主控固件 | `reconcileOTAStatus` OTA 闭环；`CheckUpdate` 按 target_chip 判可升级；告警规则按版本分流 | 版本号 + "可升级至 Vx.x.x"角标（调 CheckUpdate） |
| `firmware_esp` | 采集器固件 | 同上（target_chip=esp） | 展示 + 可升级角标 |
| `firmware_dsp` | DSP 固件 | 同上 | 展示（null 显示"无"） |
| `firmware_bms` | BMS 版本（无直连） | 落库 | 展示"无" |
| `device_type` | 设备类别 | 落库；平台设备分类统计 | 类型标签（离网逆变器） |
| `phase` | 相数 | 落库（新列）；配合 `supports_parallel` 提示并机能力 | 展示（单相） |
| `rated_power` | 额定功率（W） | **负载率** = 实时 `output_power / rated_power_w`；并联场景总额定功率求和；落库 `rated_power_w`（协议原值）并派生回填 `rated_power`(kW) | 负载率进度条卡片 + 额定功率（kW 显示） |
| `rated_voltage` | 额定电压 | 落库 | 展示 |
| `rated_frequency` | 额定频率 | 落库 | 展示 |
| `battery_nominal_voltage` | 电池标称电压 | 落库 `devices.battery_voltage` | 展示 |
| `battery_type` | 电池类型 | 落库；与 config reported 交叉校验；**充电策略提示**（LiFePO4→恒流恒压、LeadAcid→均衡提醒，与 config 均衡参数联动） | 展示 + 策略提示 |
| `cell_count` / `temp_sensor_count` | 结构信息 | 落库 | 展示 |
| `inverter_module` | 模块号（BCD） | 落库（新列，出厂追溯） | 展示 |
| `hardware_version` | 硬件版本 | 落库（列已有）；告警规则分流依据（不同硬件版本告警字典不同） | 展示 |
| `bootloader_version` | Bootloader 版本 | 落库（新列）；OTA 引导兼容性判断（bootloader 过旧先升 bootloader） | 展示 |

### 7.3 边界与约束

- **只读边界**：info 只含只读参数；可写参数（通讯地址/检测方式/法规等 V1 界面设置项）归 config 主题（见第 9 节），info 章节不包含任何可写项。
- **单位纪律**：`rated_power` 协议一律 **W**；`rated_power_w` 落库协议原值（新列）；`devices.rated_power`(kW) 仅由服务端派生回填（仅当两列都为空时按 W/1000 写入，手工录入值不被覆盖）；展示层一律 kW（÷1000），前端不自行换算。
- **上报时机**：上线 / 能力变化 / `query_info`；心跳不含只读字段，避免重复传输。
- **server 落库**：`POST /api/v1/internal/device-info` upsert（COALESCE(NULLIF)/CASE 保护模式，手工录入值不被覆盖），同时更新 `info_reported_at=NOW()` 便于排查从未上报 info 的设备。
- **缓存**：Redis `realtime:latest:{sn}.info` 组合并式写入（不覆盖遥测组），仅作即时缓存；**展示层一律从详情接口（DB）读取**。

---

## 8. 告警事件 alarm

主题 `cs_inv/{sn}/alarm` · QoS 1 · 告警发生、级别变化和恢复**立即**上报，不等待 Heartbeat。（与 V2.0 完全一致，沿用。）

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

规则：同一告警实例由 `sn + source + code` 关联；`state=0` 时 `code` 必须与对应 active 事件一致；多个源同时变化时按 fault > warning 优先级逐条上报（最多 3 条）；level 变化也作为新事件上报；位掩码由服务端按型号告警字典逐位拆分落库。

---

## 9. 配置上报 config（v2 语义键值对）

主题 `cs_inv/{sn}/config` · QoS 1 · **V2.1 起实现**（ESP32 需新增 READ_CTRLPARAM 0x01 读取控制参数 84B）。

触发：MQTT 连接成功、控制参数被写入后（CtrlParamAlterTime 变化）、收到 `query_config` 后。

### 9.1 协议格式（v=2 信封）

```json
{
  "t": 1783676930,
  "v": 2,
  "data": {
    "rev": 1783676800,
    "params": {
      "set_output_priority": 0,
      "set_max_charge_current": 200,
      "set_battery_capacity": 100,
      "set_battery_type": 0,
      "set_output_voltage": 220,
      "set_output_frequency": 50,
      "set_soc_cutoff": 20,
      "...": "..."
    }
  }
}
```

| `data` 字段 | 类型 | 必填 | 定义 |
|------|------|------|------|
| `rev` | uint32 | 是 | 控制参数修改时间（CtrlParamAlterTime，Unix 秒），服务端据此判断配置是否更新；rev 不单调递增时拒绝更新（防回退） |
| `params` | object | 是 | **42 键语义键值对**；键名与 `device_model_commands.command_code` 同源（见 9.2），值为**工程单位**（浮点或整数）；未读取到/不可用的参数键可省略 |

> **与 V2.0 差异**：V2.0 的 params 值为 ARM 原始 u16（含缩放），本版改为**工程单位**——ESP32 固件负责 0x01 读取 84B 后按参数 scale 换算为工程单位再组包，服务器与前端无需再换算；`rev` 保持 CtrlParamAlterTime 原始值。

### 9.2 42 键清单（控制参数区 0x0000-0x0029，键名与命令清单同源）

| 参数序号 | command_code（服务器键名） | 工程单位 | scale(ARM→工程) | 最小值 | 最大值 | 分组 |
|---------|--------------------------|---------|----------------|--------|--------|------|
| 0x0000 | `set_output_priority` | 枚举 | 1 | 0 | 2 | general |
| 0x0001 | `set_max_charge_current` | A | 0.1 | 0 | 60 | hybrid/charge |
| 0x0002 | `set_ac_volt_range` | 枚举 | 1 | 0 | 2 | application |
| 0x0003 | `set_battery_capacity` | Ah | 1 | 0 | 2000 | general |
| 0x0004 | `set_battery_type` | 枚举 | 1 | 0 | 2 | general |
| 0x0005 | `set_overload_restart` | bool | 1 | 0 | 1 | general |
| 0x0006 | `set_high_temp_restart` | bool | 1 | 0 | 1 | general |
| 0x0007 | `set_output_voltage` | V | 0.1 | 200 | 250 | application |
| 0x0008 | `set_output_frequency` | Hz | 0.01 | 45 | 55 | application |
| 0x0009 | `set_master_slave` | 枚举 | 1 | 0 | 1 | parallel |
| 0x000A | `set_ac_charge_current` | A | 0.1 | 0 | 150 | hybrid/charge |
| 0x000B | `set_low_volt_return_utl` | V | 0.1 | 40 | 60 | application |
| 0x000C | `set_high_volt_return_bat` | V | 0.1 | 40 | 60 | application |
| 0x000D | `set_charge_priority` | 枚举 | 1 | 0 | 2 | hybrid/charge |
| 0x000E | `set_alarm_control` | 位掩码 | 1 | 0 | 255 | general |
| 0x000F | `set_backlight_ctrl` | 枚举 | 1 | 0 | 3 | general |
| 0x0010 | `set_power_shutdown_alarm` | bool | 1 | 0 | 1 | general |
| 0x0011 | `set_overload_use_city_power` | bool | 1 | 0 | 1 | application |
| 0x0012 | `set_max_discharge_current` | A | 0.1 | 0 | 150 | hybrid/discharge |
| 0x0013 | `set_max_chg_curr` | A | 0.1 | 0 | 150 | hybrid/charge |
| 0x0014 | `set_recover_threshold_volt` | V | 0.1 | 40 | 60 | application |
| 0x0015 | `set_solar_power_balance` | bool | 1 | 0 | 1 | hybrid |
| 0x0016 | `set_ac_output_mode` | 枚举 | 1 | 0 | 2 | application |
| 0x0017 | `set_li_bat_material` | 枚举 | 1 | 0 | 1 | general |
| 0x0018 | `set_cell_serial_lifepo4` | 节 | 1 | 4 | 32 | general |
| 0x0019 | `set_cell_serial_li_nmc` | 节 | 1 | 4 | 32 | general |
| 0x001A | `set_equalize_enable` | bool | 1 | 0 | 1 | hybrid/equalize |
| 0x001B | `set_equalize_voltage` | V | 0.1 | 40 | 65 | hybrid/equalize |
| 0x001C | `set_equalize_time` | min | 1 | 0 | 720 | hybrid/equalize |
| 0x001D | `set_equalize_timeout` | min | 1 | 0 | 1440 | hybrid/equalize |
| 0x001E | `set_equalize_interval` | day | 1 | 0 | 90 | hybrid/equalize |
| 0x001F | `set_equalize_activate` | bool | 1 | 0 | 1 | hybrid/equalize |
| 0x0020 | `set_charge_time` | min | 1 | 0 | 1440 | hybrid/charge |
| 0x0021 | `set_close_charge_time` | min | 1 | 0 | 1440 | hybrid/charge |
| 0x0022 | `set_gen_start_voltage` | V | 0.1 | 40 | 60 | hybrid/gen |
| 0x0023 | `set_gen_stop_voltage` | V | 0.1 | 40 | 60 | hybrid/gen |
| 0x0024 | `set_soc_back_utl` | % | 1 | 0 | 100 | hybrid/soc |
| 0x0025 | `set_soc_back_bat` | % | 1 | 0 | 100 | hybrid/soc |
| 0x0026 | `set_soc_back_gen` | % | 1 | 0 | 100 | hybrid/soc |
| 0x0027 | `set_soc_close_gen` | % | 1 | 0 | 100 | hybrid/soc |
| 0x0028 | `set_soc_cutoff` | % | 1 | 0 | 100 | hybrid/soc |
| 0x0029 | `set_buzzer` | bool | 1 | 0 | 1 | general |

> **互斥约束**（服务端校验）：`set_soc_cutoff ≤ set_soc_back_utl ≤ set_soc_back_gen`；`set_gen_start_voltage ≥ set_gen_stop_voltage`；`set_low_volt_return_utl ≤ set_high_volt_return_bat`；不通过时对应键标记 QualityFlags，不阻断整包。
> 0x002A-0x0032 与 0x0052/0x0053（S/A/B 级 SOC、手动/控制插座、恢复阈值 SOC、锂电协议、发电机额定功率、发电机平衡、市电充电截止 SOC/电压）为扩展参数区，**固件支持读取时一并上报**（键名：`set_s_grade_soc`/`set_a_grade_soc`/`set_b_grade_soc`/`set_manual_socket`/`set_control_socket`/`set_recover_threshold_soc`/`set_li_bat_protocol_type`/`set_gen_rate_watt`/`set_ge_balance_en`/`set_soc_max_utl_chg`/`set_vmax_utl_chg`），未实现前可省略；`set_utc_time`（0x00CA）由 SNTP 自动执行，**不在 config 上报范围内**。

### 9.3 服务端处理：desired/reported 闭环

- 上行 config 经 `ParseReportedConfigV2` 解析 → schema 校验（范围/枚举/互斥）→ 写入 `device_control_state.reported` + `reported_revision`（仅当新 rev ≥ 旧 rev）。
- 与 `desired` 比对更新 `sync_status`：完全一致 → `synced`；不一致 → `drifted`（生成 `CONFIG_DRIFT` 诊断事件）；desired 为空 → `unknown`。
- **禁止用下发值覆盖实际值**：reported 只来自设备上报。
- 审计：参数值相对上一次上报发生变化时，写 `device_config_changes`（source=reported）。

### 9.4 配置漂移（CONFIG_DRIFT）

当 `sync_status=drifted` 持续超过 1 个心跳周期（180s），诊断引擎生成 `CONFIG_DRIFT`（level=warning，detail 含漂移参数清单 desired vs reported），写入 `device_diagnostics`；恢复一致后自动关闭（status=resolved）。

---

## 10. 网络状态 status / LWT

主题 `cs_inv/{sn}/status` · QoS 1 · Retain（现有 ESP32 实现，沿用不改。与 V2.0 完全一致。）

- **上线**（MQTT 连接成功）：立即发布一次

```json
{"t": 1783676930, "v": 2, "data": {"online": true, "rssi": -45, "ip": "192.168.1.100"}}
```

- **LWT 遗嘱**（broker 在异常断开时代发，Retain）：

```json
{"t": 0, "v": 2, "data": {"online": false}}
```

- 不做 60s 周期刷新；业务健康（work_state、故障、数据新鲜度）由 heartbeat 判断，与网络在线状态分开。
- 在线判定（服务端）：超过 420s 无有效上行数据即判定离线。

---

## 11. 控制命令与命令闭环

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
| `cmd` | string | 是 | 命令名（与 config 42 键同源的 `command_code`，见 9.2） |
| `args` | array | 是 | 参数数组；本协议命令均为单值命令，取 `args[0]`；查询类命令为 `[]` |

> **兼容性**：ESP32 解析器同时兼容旧格式 `{"cmd": "...", "value": <number>}`（及 `name`/`action`、`param` 别名），便于过渡期服务器双发；新服务器必须使用标准信封。

### 11.3 命令清单

命令清单 = **42 个控制参数命令（第 9.2 表，键名即 command_code）+ 3 个查询命令 + 时间命令**，共 46 条，注册于 `device_model_commands`（迁移 091 已注册 27 条，迁移 096 补齐 15 条控制参数命令 + 3 条查询命令）。

**查询命令**（args 为空数组，无参数）：

| command_code | 触发行为 |
|--------------|---------|
| `query_telemetry` | 立即补报一次 heartbeat（180s 周期重置） |
| `query_info` | 立即补报一次 info（驱动管理后台"刷新设备信息"按钮） |
| `query_config` | 立即补报一次 config |

**时间命令**：

| command_code | 触发行为 |
|--------------|---------|
| `set_utc_time` | UTC 时间同步（写 32 位，差值>4s 校时）；**固件由 SNTP 模块自动执行（每日一次），无需云端下发** |

### 11.4 命令响应（cs_inv/{sn}/cmd/response）与命令闭环

```json
{
  "t": 1783676930,
  "v": 2,
  "data": {
    "task_id": "019f5b9e-7bd7-7e50-a14d-5c8a74235a10",
    "cmd": "set_max_charge_current",
    "result": "OK",
    "err": 0,
    "applied_args": [50],
    "reported_revision": 1783676800
  }
}
```

| 字段 | 类型 | 定义 |
|------|------|------|
| `task_id` | string | 回显下行 task_id；旧格式命令无 task_id 时省略 |
| `cmd` | string | 命令名 |
| `result` | string | `OK` / `INVALID_ARGS` / `NOT_SUPPORTED` / `BUSY` / `EXPIRED` / `EXEC_FAILED`（枚举沿用现有） |
| `err` | int | 0=成功；-1 执行失败；-2 参数错误；-3 忙；-4 不支持；-5 越界；-6 过期 |
| `applied_args` | array | **V2.1 新增**：ARM 实际采纳的参数值（工程单位，写失败时可为空）；服务端回填命令日志 |
| `reported_revision` | uint32 | **V2.1 新增**：命令生效后最新的 CtrlParamAlterTime；服务端据此判断该命令是否已反映到 reported |

**命令闭环（服务端）**：

1. 命令完成（OK 或带 err）后，将 `applied_args` 回填命令日志，并立即向设备下发 `query_config` 索取最新配置。
2. 收到新 config 后按 9.3 更新 reported/sync_status；命令"已生效"判定条件 = `sync_status=synced` 或 `reported_revision ≥ 命令下发时 rev`。
3. 命令响应 `result_code` 拒绝码枚举：`OK=0`、`INVALID_ARGS=-2`、`NOT_SUPPORTED=-4`、`BUSY=-3`、`EXPIRED=-6`、`EXEC_FAILED=-1`（与 `err` 数值一致，服务端统一映射）。

> 命令在 2 秒内应答；ARM 写响应 `err!=0` 或 800ms 超时 → `EXEC_FAILED`（err=-1）。

---

## 12. 服务器解析链路

```
ESP32 --MQTT--> EMQX --webhook--> mqtt-kafka-bridge --Kafka(inv-telemetry)--> ProtocolParser
    → 按 topic + v 分派 → v==2 走 ParseHeartbeatV2（heartbeat 主题，57 值自适应）
    → SaveTelemetryV2（device_telemetry_3min 新列）→ 诊断引擎（独立写入，失败不阻塞）
    → Redis realtime:latest:{sn}（前端实时展示）
    → 状态机（在线/故障检测）
```

### 12.1 解析要点

- `ProtocolParser` 对 `heartbeat` 主题先读信封 `v` 字段：`v==1` 走 `ParseHeartbeat`（V1 数组），`v==2` 走 `ParseHeartbeatV2`（本协议 6.2 数组，57 值自适应）。
- V2 解析器复用 V1 的校验模式：组长度精确校验（六组 + fan/diag/sock 自适应）、数值或 null 校验、范围 bounded、QualityFlags（NullValue/OutOfRange/ClockInvalid）。
- `work_state` / `battery_power` 按 6.3 推导写入对应列。
- 新增 8 列写入 `device_telemetry_3min` 与 `device_latest_state`（见 13.4）。
- Redis `realtime:latest:{sn}` 按组结构缓存（字段还原为工程单位），新增 fan/diag/sock 组与派生（parallel/health/thermal，见 12.4）。

### 12.2 info 处理（V2.1 扩展）

- `DeviceInfo` 结构补 4 字段：`phase`、`inverter_module`、`hardware_version`、`bootloader_version`（与业务侧 `internalDeviceInfoRequest` 三处 JSON 契约同步，见 12.5）。
- 解析保持现有容错（嵌套/扁平 payload、空值容忍、失败仅记 ingest error 不阻塞 heartbeat 主链路）；沿用异步注册队列 3 次退避重试。
- Redis `realtime:latest:{sn}.info` payload 同步加 4 键；展示层以 DB 为权威。

### 12.3 诊断与健康度

`SaveTelemetryV2` 成功后触发诊断引擎（独立写入 `device_diagnostics` / `device_health_history`，失败不阻塞主链路）；规则与阈值见第 14 节，阈值参数化存放在 `device_models.specifications` jsonb。

### 12.4 Redis realtime 组结构（V2.1）

`realtime:latest:{sn}` 在 V2.0 组结构（sys/pv/ac/chr/bat/eng + info 合并）基础上新增：

```json
{
  "fan": {"data": {"mppt_fan_speed": 88, "inv_fan_speed": 76}, "timestamp": 1783000000},
  "diag": {"data": {"inv_current": 213.0, "parallel_charge_current": 45, "work_time_total": 68400000}, "timestamp": 1783000000},
  "sock": {"data": {"paired_socket": 3, "online_socket": 2, "on_socket": 2}, "timestamp": 1783000000},
  "derived": {
    "data": {
      "parallel_role": "master",
      "parallel_health": "degraded",
      "thermal_status": "normal",
      "health_score": 85,
      "health_level": "good"
    },
    "timestamp": 1783000000
  }
}
```

### 12.5 三处 JSON 契约同步

| 位置 | 变更 |
|------|------|
| `device-communication/internal/model/device.go` `DeviceInfo` | 补 `Phase`/`InverterModule`/`HardwareVersion`/`BootloaderVersion` |
| `device-communication/internal/service/protocol_parser.go` `internalDeviceInfoRequest`（L61-78） | 同步补 4 字段；handleInfo 映射与 Redis payload 同步加 4 键 |
| `business-api/internal/handler/internal_handler.go` `internalDeviceInfoRequest`（L33-49） | 同步补 4 字段；upsert SQL（L435-467）补新列 |

---

## 13. 数据库型号配置（迁移 096）

迁移脚本 `database/migrations/096_add_csl10_6k2_v21.up.sql` 完成以下变更（092-095 已被占用，故编号 096；幂等可重放）：

### 13.1 修正 091 偏差（以本文档为准）

1. `device_protocol_fields` 键名统一：ac[0]-ac[4] → `ac_output_voltage`/`ac_output_frequency`/`output_power`/`output_apparent_power`/`output_current`（catalog 新增对应键）；eng[2]/eng[3]/eng[6]-eng[9] → `daily_pv_energy`/`total_pv_energy`/`daily_discharge_energy`/`total_discharge_energy`/`daily_charge_energy`/`total_charge_energy`（catalog 既有键）。
2. 删除 bat[5] 冗余位（`battery_overcharge` 仅 sys[10] 一处）。
3. `battery_soc` scale 0.1 → 1（`device_protocol_fields` + `telemetry_field_catalog` 同步）。
4. 注意：catalog 被 `device_protocol_fields`/`device_model_fields` 外键 RESTRICT 引用，先 INSERT 新键，再 UPDATE 引用，最后 DELETE 旧引用。

### 13.2 心跳 schema 更新

- `device_protocol_versions` heartbeat v2 的 `schema_hash` 更新为 `heartbeat-v2-csl10-6k2-v2.1-20260805`。
- `device_protocol_fields` 新增 8 个位置：fan[0]/fan[1]/diag[0]/diag[1]/diag[2]/sock[0]/sock[1]/sock[2]（fan/diag float32、`work_time_total` float64、sock uint32）。
- `telemetry_field_catalog` 新增 8 字段：`mppt_fan_speed`/`inv_fan_speed`（float，%）、`inv_current`（float，A）、`parallel_charge_current`（float，A）、`work_time_total`（float，s）、`paired_socket`/`online_socket`/`on_socket`（integer，位掩码）。

### 13.3 设备信息列

- `devices` 新增：`phase VARCHAR(20) DEFAULT ''`、`inverter_module VARCHAR(64) DEFAULT ''`、`bootloader_version VARCHAR(50) DEFAULT ''`、`rated_power_w INTEGER DEFAULT 0`（协议原值 W，与 `rated_power`(kW) 语义隔离）、`info_reported_at TIMESTAMPTZ`。
- 索引：`idx_devices_model ON devices(model) WHERE deleted_at IS NULL`（OTA 按型号筛选）。

### 13.4 遥测落库列

- `device_telemetry_3min` + `device_latest_state` 新增 8 列：`mppt_fan_speed`/`inv_fan_speed`/`inv_current`/`parallel_charge_current` REAL；`work_time_total` DOUBLE PRECISION；`paired_socket`/`online_socket`/`on_socket` INTEGER。
- `device_latest_state` 新增 `health_score REAL`。

### 13.5 新表

- `device_diagnostics`（device_sn, rule_code, level, status, detail jsonb, first_at, last_at, count，主键 sn+rule_code）——诊断事件聚合。
- `device_health_history`（device_sn, event_time, score, level, factors jsonb）——健康度历史。
- `device_config_schema`（param_key 主键, group_code, sub_group, control_type, scale, unit, min, max, enum_map jsonb, step, permission_code, confirmation_mode, display_name_key, sort_order, visibility jsonb, validation jsonb）——42 个控制参数全部入表；`group_code` 对齐 V1 界面四分组（general/application/hybrid/parallel），hybrid 下 `sub_group` 细分 charge/discharge/soc/equalize/gen。
- `device_config_changes`（id, device_sn, param_key, old_value, new_value, source, rev, changed_at, task_id）——配置审计。

### 13.6 型号与命令

- `device_models`：CS-L10-6K2 `supports_parallel=TRUE`（sock 组有数据源）；`specifications` jsonb 扩展诊断/健康度阈值（见 14.5）。
- `device_model_commands` 加列 `config_domain`、`permission_code`、`confirmation_mode`、`operation_kind`；补 15 条控制参数命令（9.2 表）与 3 条查询命令 `query_info`/`query_telemetry`/`query_config`（parameter_schema `[]`、risk_level 1）。
- `device_model_fields` 注册新 8 遥测字段（分组 fan/diag/sock）+ **info 组静态字段配置**（field_key=`hardware_version`/`phase`/`bootloader_version`/`inverter_module`/`rated_power_w` 等，group_code='info'、show_realtime=false——App 端 `_normalizeGroupName` 已支持 info→device_info 分组渲染，DB 配置即生效）。

---

## 14. 利用层规则：诊断与健康度

> 阈值（70°C/30%/85°C/5000h/扣分项）为建议值，参数化存放在 `device_models.specifications` jsonb，**待厂家签字**后可调。诊断事件写入 `device_diagnostics`（去重聚合 first_at/last_at/count），活跃诊断同步 Redis，恢复后 status=resolved。

### 14.1 散热诊断

| rule_code | 触发条件 | level | detail |
|-----------|---------|-------|--------|
| `INV_FAN_ABNORMAL` | `inv_fan_speed < 30%` 且 `inverter_temperature > 70°C` | fault | 逆变风扇转速/温度采样值 |
| `MPPT_FAN_ABNORMAL` | `mppt_fan_speed < 30%` 且 `boost_temperature > 70°C` | fault | MPPT 风扇转速/温度采样值 |
| `THERMAL_OVERHEAT` | `inverter_temperature > 85°C` 或 `boost_temperature > 85°C` | fault | 过温采样值 |

恢复条件：触发条件不成立（持续 3 个心跳周期即 540s 内均不成立）→ status=resolved。

### 14.2 并机诊断

| rule_code | 触发条件 | level | detail |
|-----------|---------|-------|--------|
| `PARALLEL_SLAVE_OFFLINE` | `paired_socket > 0` 且 `online_socket < paired_socket` | warning | paired/online 掩码与差集 |
| `PARALLEL_SLAVE_NOT_RUNNING` | `online_socket > on_socket` | info | 在线但未运行的插座掩码 |

### 14.3 维护提醒

| rule_code | 触发条件 | level | detail |
|-----------|---------|-------|--------|
| `MAINTENANCE_DUE` | `work_time_total` 跨过 5000h（18,000,000s）阈值 | warning | 累计运行时长；阈值参数化，支持 5000/10000h 多档 |

判定：上次采样 < 阈值 ≤ 本次采样（跨过即触发，不重复触发，除非阈值提升）。

### 14.4 健康度评分

基础分 100，按以下扣分项计算（写 `device_latest_state.health_score` + `device_health_history`）：

| 扣分项 | 条件 | 扣分 |
|--------|------|------|
| 活跃故障 | 存在 status=active 的 fault 级诊断 | -30 |
| 活跃告警 | 存在 status=active 的 warning 级诊断 | -10 |
| 温度过高 | `inverter_temperature > 75°C` 或 `boost_temperature > 75°C` | -15 |
| 风扇异常 | 任一风扇转速 < 30% | -15 |
| 低电量 | `battery_soc < 20%` | -10 |
| 并机掉线 | `PARALLEL_SLAVE_OFFLINE` 活跃 | -5 |

分级：≥90 **健康** / 70-89 **良好** / 50-69 **注意** / <50 **需维护**。分数下限 0。

### 14.5 阈值参数化（specifications jsonb）

```json
{
  "phase": "single",
  "grid_mode": "off_grid",
  "uart_protocol": "collector_v1",
  "diagnostics": {
    "fan_speed_low_percent": 30,
    "fan_abnormal_temp_c": 70,
    "overheat_temp_c": 85,
    "health_temp_high_c": 75,
    "maintenance_hours": 5000,
    "deduct_fault": 30,
    "deduct_warning": 10,
    "deduct_temp_high": 15,
    "deduct_fan_abnormal": 15,
    "deduct_low_soc": 10,
    "deduct_parallel_offline": 5
  }
}
```

---

## 15. ESP32 固件规范

> 固件仓库（esp32c3_l10_idf）另行实施，本协议为唯一事实源。

### 15.1 心跳组包（57 值）

- 读运行参数 0x03 READ_RUNPARAM（0x03E8-0x0442，176B）→ 88 个 u16 → 按 6.2 映射组包。
- 新增组映射（ARM 地址确认后实现）：fan ← MpptFanSpeed/InvFanSpeed；diag ← InvCurrent/ParallelChgCurr/WorkTimeTotal；sock ← PairedSocket/OnlineSocket/OnSocket。
- `sys[6]`/`sys[7]`：ARM 已赋值则如实上报（含缩放原始值），未赋值填 `null`。
- 信封 `{"v":2,"t":<unix>,"data":{...}}`；组顺序/长度严格按 6.1；数值为 ARM 原始量纲。
- 周期 180s + 启动立即 + `query_telemetry` 补报（周期重置）。

### 15.2 config 组包（v2 语义键值对）

- 0x01 READ_CTRLPARAM 读 0x0000-0x0053（84B）→ 42 个 u16 → 按 9.2 表 scale 换算为**工程单位** → 组包 `{"v":2,"t":...,"data":{"rev":<CtrlParamAlterTime>,"params":{...}}}`。
- `rev` 取 0x00C8-0x00C9 的 CtrlParamAlterTime（u32，Unix 秒），原始值不换算。
- 键名 = 9.2 表 command_code；未读到的键省略；不可用键不发送。
- 触发：连接成功、写入命令生效后（检测 CtrlParamAlterTime 变化）、收到 `query_config`。

### 15.3 info 组包（17 字段）

- 0x06 READ_R_DATA 读 0x0190-0x01A9（52B）→ 26 个 u16 → 按 7.1 表格式化（版本号 `V%d.%d`、模块号 BCD、额定值缩放）→ 组包 17 字段（含 `phase`/`inverter_module`/`hardware_version`/`bootloader_version` 4 个 V2.1 透传字段）。
- `battery_type` 由控制参数 0x0004 映射（0→LiFePO4、1→NCM、2→LeadAcid、其他→Other），未读取前按出厂默认 LiFePO4。
- 触发：上线（MQTT 连接成功且已有只读数据）、任一字段变化、收到 `query_info`。

### 15.4 cmd/response 拒绝码

- 下行信封解析兼容标准格式 `{"v":2,"task_id":...,"t":...,"cmd":...,"args":[...]}` 与旧格式 `{"cmd":...,"value":...}`。
- 响应 data 扩展 `applied_args`（ARM 实际采纳的工程单位值）与 `reported_revision`（命令生效后最新 CtrlParamAlterTime）。
- 拒绝码（`err`）：0 OK；-1 EXEC_FAILED（ARM 写响应 err!=0 或 800ms 超时）；-2 INVALID_ARGS；-3 BUSY；-4 NOT_SUPPORTED；-5 OUT_OF_RANGE；-6 EXPIRED。
- 命令 2 秒内应答。

### 15.5 查询命令

| command_code | 行为 |
|--------------|------|
| `query_telemetry` | 立即补报 heartbeat（180s 周期重置） |
| `query_info` | 立即补报 info |
| `query_config` | 立即补报 config |

---

## 16. 与 V2.0 修订对照

| # | V2.0 | 问题/变更 | V2.1 修订 |
|---|------|----------|-----------|
| 1 | 心跳 49 值（六组），剔除 fan/runtime/inv_current 等 | 固件确认 ARM 有数据源；用户要求全部上报 | **57 值**：新增 fan[2]/diag[3]/sock[3] 共 8 位置，六组位置不动（纯 additive）；49 值旧固件自适应兼容 |
| 2 | `sys[6]`/`sys[7]` 变压器/PV 温度标注"ARM 未赋值 → null" | 需如实上报 | 位置保留，ARM 赋值后如实上报，未赋值填 null |
| 3 | 14.3 剔除清单（fan_speed/runtime_hours 等） | 全部回归 | 删除该剔除项，回归字段见 6.2.7-6.2.9 |
| 4 | ac[0]-ac[4] 键名 `ac_output_voltage` 等（091 误用 V1 键名） | 091 迁移与文档不一致 | **文档键名保留**，096 修正 `device_protocol_fields`/catalog/`device_model_fields` |
| 5 | ac[9]/ac[10] 键名 `ac_discharge_*` | 091 已按 `ac_bypass_*` 实现（落库列/代码/前端字段配置） | **键名定为 `ac_bypass_power`/`ac_bypass_apparent_power`**（语义不变：交流放电功率） |
| 6 | eng[2]/eng[3]/eng[6]-eng[9] 键名 `pv_energy_daily` 等 | 与 catalog 既有 V1 字段重复 | **采用 catalog 既有键名**（`daily_pv_energy` 等），096 修正 protocol_fields 键名 |
| 7 | `battery_soc` scale 0.1（091 错误） | 文档为 1% | 096 修正 scale=1 |
| 8 | bat[5] 冗余位（091 保留） | 文档已删 | 096 删除 |
| 9 | schema_hash `heartbeat-v2-csl10-6k2-20260802` | 组包升级 | 更新为 `heartbeat-v2-csl10-6k2-v2.1-20260805` |
| 10 | info 17 字段仅落库 | 用户要求"很好地利用" | **业务化**：7.2 映射表（负载率/OTA/型号能力/电池策略/前端展示）；补 4 字段透传（phase/inverter_module/hardware_version/bootloader_version 已定稿，本版补实现契约 12.5） |
| 11 | info 上报链路无 `info_reported_at` | 无法排查未上报设备 | devices 新增 `info_reported_at` 列 |
| 12 | `rated_power` 单位 W 与 `devices.rated_power`(kW) 冲突 | upsert 直存单位混乱 | 新增 `rated_power_w` 列存协议原值；`rated_power`(kW) 仅服务端派生回填；展示层 ÷1000 |
| 13 | config 42 键平铺（V2.0 9 节，值为 ARM 原始 u16） | 用户要求"不能 42 个 u16 平铺" | **重写**：v=2 语义键值对（工程单位）+ rev；`device_config_schema` 表驱动；desired/reported 闭环（9.3）；CONFIG_DRIFT 诊断（9.4）；审计 `device_config_changes` |
| 14 | config 无 schema 校验 | — | 范围/枚举/互斥校验，不通过标记 QualityFlags |
| 15 | 无诊断/健康度 | 用户要求"很好地利用" | **新增第 14 节**：散热/并机/维护诊断 + 健康度评分；`device_diagnostics`/`device_health_history` 新表 |
| 16 | 命令响应无 applied_args/reported_revision | 无法判断命令是否生效 | 11.4 扩展 + 命令闭环（query_config 兜底） |
| 17 | `supports_parallel=FALSE`（091） | sock 组有数据源 | 096 更新 `supports_parallel=TRUE` |
| 18 | 查询命令未注册（091 仅 27 条控制命令） | App/后台"刷新"按钮需要 | 096 补 3 条查询命令 + 15 条控制参数命令（共 45 条控制/查询） |
| 19 | 管理后台配置页为命令卡片平铺 | 用户要求 V1 风格分组 | SchemaGroupPanel：四分组（通用/应用/混合/并联）+ 子分组 + 即点即发 + 联动 + 危险确认 + 闭环状态（前端阶段） |
| 20 | 设备详情无健康/诊断/信息 Tab | — | 管理后台新增"健康诊断"/"设备信息"Tab（前端阶段） |

---

## 17. 实施清单与验证

| # | 项 | 文件 | 状态 |
|---|----|------|------|
| 1 | 协议设计文档（本版） | `docs/CS-L10-6K2_MQTT_上报协议设计_V2.1.md` | 本文档 |
| 2 | 数据库迁移 | `database/migrations/096_add_csl10_6k2_v21.up.sql` / `.down.sql` | 待实施 |
| 3 | V2 解析器 57 值扩展 | `device-communication/internal/telemetry/heartbeat_v2.go` | 待实施 |
| 4 | Sample 扩展（Fan/Diag/Sock） | `device-communication/internal/telemetry/model.go` | 待实施 |
| 5 | info 4 字段透传 | `device-communication/internal/model/device.go` + `service/protocol_parser.go` + `business-api/internal/handler/internal_handler.go` | 待实施 |
| 6 | 落库扩展（8 列 + health_score） | `device-communication/internal/repository/telemetry_repository.go` | 待实施 |
| 7 | 诊断引擎 + 健康度 | `device-communication/internal/service/diagnostic_engine.go` / `health_score.go` | 待实施 |
| 8 | config v2 解析 + 命令闭环 | `device-communication/internal/telemetry/config_v2.go` + `service/protocol_parser.go` | 待实施 |
| 9 | 详情接口业务派生（load_percent/ota_available） | `business-api/internal/handler/device_handler.go` + `model/models.go` | 待实施 |
| 10 | 单元测试 | 57 值 / info / upsert / config / 诊断 / 健康度 | 待实施 |
| 11 | 管理后台：健康诊断 Tab + InfoTab + SchemaGroupPanel + 列表单位修正 | `inv-admin-frontend/src/pages/device-detail/`、`remote-settings/`、`devices/index.tsx` | 待实施 |
| 12 | App 元数据驱动（info 组展示） | `inv_app/`（DB 配置生效，必要时补 Dart 展平） | 待实施 |
| 13 | **ESP32 固件：心跳 57 值组包** | `esp32c3_l10_idf/main/telemetry/telemetry.c` | 待实施 |
| 14 | **ESP32 固件：config 组包（0x01 读 84B → 42 键工程单位）** | `esp32c3_l10_idf/main/telemetry/` | 待实施 |
| 15 | **ESP32 固件：info 组包（0x06 读 0x0190-0x01A9 → 17 字段）** | `esp32c3_l10_idf/main/telemetry/telemetry.c` | 待实施 |
| 16 | **ESP32 固件：cmd/response 扩展（applied_args/reported_revision）+ 拒绝码** | `esp32c3_l10_idf/main/telemetry/cmd_handler.c` | 待实施 |

验证命令：

```bash
make build-device && go test ./device-communication/... && make vet-go
make build-api && go test ./business-api/... && make vet-go
# 迁移 096 在测试库执行：psql -f database/migrations/096_add_csl10_6k2_v21.up.sql
# 前端：make type-check && make lint-web
# 模拟器验证：heartbeat 57 值、info 17 字段（含 4 新字段透传与 rated_power_w 落库）、config 42 键、诊断事件生成、健康度落库
```
