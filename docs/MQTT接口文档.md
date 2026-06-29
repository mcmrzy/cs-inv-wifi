# CS-I10-6k2 MQTT 接口文档

## 一、概述

本文档定义 CS-I10-6k2 单相离网 48V 逆变器 WiFi 通信模块的 MQTT 接口规范，涵盖连接参数、数据上行/下行协议、心跳机制及 OTA 升级流程。

**通信架构**：

```
云端 Broker ←── MQTT ──→ ESP32 ←── UART ──→ ARM
```

## 二、术语说明

| 缩写 | 说明 |
|------|------|
| SN | 设备序列号，16 位，格式 `CS-SN-STD-001 V1.1规范` |
| ESP32 | WiFi 通信模块，负责 MQTT 连接与数据透传 |
| ARM | 逆变器主控芯片，负责采集数据与执行控制 |
| Broker | MQTT 消息服务器 |
| LWT | Last Will and Testament，遗嘱消息 |

---

## 三、MQTT 连接参数

| 参数 | 值 | 说明 |
|------|-----|------|
| 协议 | `mqtts://` | TLS 加密（端口 8883） |
| 端口 | 8883 | TLS 端口（默认） |
| Keepalive | 60s | 保活间隔 |
| Client ID | 设备 SN | 16 位 CS-SN-STD-001 V1.1 规范|
| Username | `CSKJ-INV-DEVICE-6K2` | 固定用户名 |
| Password | `CSKJINVDEVICE6K2` | 固定密码 |
| Clean Session | false | 非清理会话 |
| 默认 Broker | `jiuxiaoyw.online:8883` | 可通过配网页面修改 |

---

## 四、主题格式

```
cs_inv/{设备SN}/{子主题}
```

示例：`cs_inv/H1CNA00135000014/data/ac`

---

## 五、数据上行（设备 → 云端）

### 5.1 ARM 数据发布流程

ARM 通过 UART `CMD=0x02` 发送 JSON 到 ESP32，ESP32 透传到 MQTT：

**ARM → ESP32 帧格式**：

```
CMD=0x02
DATA={"sn":"H1CNA00135000014","topic":"data/ac","payload":"{\"voltage\":220.5,...}"}
```

**ESP32 → 云端**：

```
主题: cs_inv/H1CNA00135000014/data/ac
Payload: {"data":{"voltage":220.5,...},"timestamp":1716800000}
```

> **注意**：ESP32 会为所有上报数据添加 Unix 时间戳（秒），无论在线发送还是离线缓存发送，数据格式完全统一。时间戳由 ESP32 的 NTP 同步时间提供。

### 5.2 上行主题总览

| 主题 | QoS | Retain | 频率 | 来源 | 说明 |
|------|-----|--------|------|------|------|
| `cs_inv/{sn}/status` | 1 | true | 60s | ESP32 自动 | 在线状态 + RSSI + IP |
| `cs_inv/{sn}/info` | 1 | false | 连接时 | ARM | 设备信息（上电一次） |
| `cs_inv/{sn}/data/ac` | 0 | false | 5s | ARM | 交流输出 |
| `cs_inv/{sn}/data/battery` | 0 | false | 5s | ARM | 电池 BMS |
| `cs_inv/{sn}/data/pv` | 0 | false | 5s | ARM | 光伏 MPPT |
| `cs_inv/{sn}/data/status` | 0 | false | 5s | ARM | 逆变器系统状态 |
| `cs_inv/{sn}/data/energy` | 0 | false | 60s | ARM | 能量统计 |
| `cs_inv/{sn}/data/cells` | 0 | false | 30s | ARM | 电芯详细数据 |
| `cs_inv/{sn}/data/parallel` | 0 | false | 5s | ARM | 并机信息 |
| `cs_inv/{sn}/data/three_phase` | 0 | false | 5s | ARM | 三相数据 |
| `cs_inv/{sn}/data/control` | 0 | false | 5s | ARM | 远程控制状态 |
| `cs_inv/{sn}/data/alarm` | 1 | false | 事件触发 | ARM | 告警/故障事件 |
| `cs_inv/{sn}/cmd/response` | 1 | false | 按需 | ARM | 控制命令执行结果 |
| `cs_inv/{sn}/ota/status` | 1 | false | 按需 | ESP32 | OTA 升级状态 |

### 5.3 各主题 Payload 格式


#### status（ESP32 自动生成，非 ARM 上报）

频率：60s

```json
{
  "online": true,
  "rssi": -45,
  "ip": "192.168.1.100"
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `online` | bool | 设备在线状态 |
| `rssi` | int | WiFi 信号强度 (dBm) |
| `ip` | string | 设备本地 IP 地址 |

#### info（ARM 上报，连接时一次）

频率：连接时上报一次

```json
{
  "sn": "H1CNA00135000014",
  "model": "CS-I10-6k2",
  "manufacturer": "辰烁科技",
  "firmware_arm": "V1.2.3.20240510",
  "firmware_esp": "V1.0.5.20240420",
  "firmware_dsp": "V1.1.0.20240508",
  "firmware_bms": "V2.0.1.20240415",
  "type": "离网逆变器",
  "rated_power": 6200,
  "rated_voltage": 220,
  "rated_freq": 50.0,
  "battery_voltage": 51.2,
  "battery_type": "LiFePO4",
  "cell_count": 16,
  "parallel_mode": "standalone",
  "parallel_role": "standalone",
  "parallel_phase": null
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `sn` | string | 设备序列号（16 位） |
| `model` | string | 设备型号 |
| `manufacturer` | string | 制造商名称 |
| `firmware_arm` | string | ARM 固件版本号 |
| `firmware_esp` | string | ESP32 固件版本号 |
| `firmware_dsp` | string | DSP 固件版本号 |
| `firmware_bms` | string | BMS 固件版本号 |
| `type` | string | 设备类型 |
| `rated_power` | int | 额定功率 (W) |
| `rated_voltage` | int | 额定输出电压 (V) |
| `rated_freq` | float | 额定输出频率 (Hz) |
| `battery_voltage` | float | 电池额定电压 (V) |
| `battery_type` | string | 电池类型（如 `LiFePO4`） |
| `cell_count` | int | 电芯串数 |
| `parallel_mode` | string | 并机模式：`standalone`、`parallel`、`three_phase` |
| `parallel_role` | string | 并机角色：`standalone`、`master`、`slave` |
| `parallel_phase` | string/null | 三相角色：`L1`、`L2`、`L3`，独立模式为 null |

#### data/ac（交流输出）

频率：5s

```json
{
  "voltage": 220.5,
  "current": 8.52,
  "power": 1870.3,
  "apparent": 1875.6,
  "frequency": 50.02,
  "pf": 0.997,
  "load_percent": 30.2,
  "thd_v": 1.8
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `voltage` | float | 交流输出电压 (V) |
| `current` | float | 交流输出电流 (A) |
| `power` | float | 有功功率 (W) |
| `apparent` | float | 视在功率 (VA) |
| `frequency` | float | 输出频率 (Hz) |
| `pf` | float | 功率因数（0~1，**必须 ≤ 1**） |
| `load_percent` | float | 负载率 (%) |
| `thd_v` | float | 电压 THD (%) |

#### data/battery（电池 BMS）

频率：5s

```json
{
  "soc": 78.5,
  "soh": 96.2,
  "voltage": 51.2,
  "current": 25.5,
  "power": 1305.6,
  "capacity_remain": 78.5,
  "capacity_total": 100.0,
  "cycle_count": 152,
  "temp_max": 28.5,
  "temp_min": 25.0,
  "cell_volt_max": 3.35,
  "cell_volt_min": 3.28,
  "cell_volt_diff": 0.07,
  "charge_state": 1,
  "protect_status": 0,
  "bms_fault_code": 0,
  "max_chg_current": 60.0,
  "max_dischg_current": 120.0,
  "charge_volt_ref": 54.6,
  "dischg_cut_volt": 44.0,
  "temp_battery": 26.5
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `soc` | float | 电池电量百分比 (%) |
| `soh` | float | 电池健康度 (%) |
| `voltage` | float | 电池电压 (V) |
| `current` | float | 电池电流 (A)，正值充电，负值放电 |
| `power` | float | 充放电功率 (W)，正=充电，负=放电 |
| `capacity_remain` | float | 当前剩余容量 (Ah) |
| `capacity_total` | float | 标称额定容量 (Ah) |
| `cycle_count` | int | 累计充放电循环次数 |
| `temp_max` | float | 电芯最高温度 (℃) |
| `temp_min` | float | 电芯最低温度 (℃) |
| `cell_volt_max` | float | 单体最高电压 (V) |
| `cell_volt_min` | float | 单体最低电压 (V) |
| `cell_volt_diff` | float | 电芯压差 (V) |
| `charge_state` | int | 充电状态：0=静置、1=充电、2=放电 |
| `protect_status` | int | 保护状态位掩码 |
| `bms_fault_code` | int | BMS 故障码，0=正常 |
| `max_chg_current` | float | 最大允许充电电流 (A) |
| `max_dischg_current` | float | 最大允许放电电流 (A) |
| `charge_volt_ref` | float | 充电目标电压 (V) |
| `dischg_cut_volt` | float | 放电截止电压 (V) |
| `temp_battery` | float | 电池平均温度 (℃) |

#### data/pv（光伏 MPPT）

频率：5s

```json
{
  "pv1_voltage": 85.3,
  "pv1_current": 12.5,
  "pv1_power": 1066.3,
  "pv1_voltage_max": 100.0,
  "pv1_power_max": 1200.0,
  "pv2_voltage": 0.0,
  "pv2_current": 0.0,
  "pv2_power": 0.0,
  "pv2_voltage_max": 0.0,
  "pv2_power_max": 0.0,
  "pv_power_total": 1066.3,
  "mppt_state": "tracking"
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `pv1_voltage` | float | 第一路光伏输入电压 (V) |
| `pv1_current` | float | 第一路光伏输入电流 (A) |
| `pv1_power` | float | 第一路光伏输入功率 (W) |
| `pv1_voltage_max` | float | 第一路光伏历史最高电压 (V) |
| `pv1_power_max` | float | 第一路光伏历史最高功率 (W) |
| `pv2_voltage` | float | 第二路光伏输入电压 (V) |
| `pv2_current` | float | 第二路光伏输入电流 (A) |
| `pv2_power` | float | 第二路光伏输入功率 (W) |
| `pv2_voltage_max` | float | 第二路光伏历史最高电压 (V) |
| `pv2_power_max` | float | 第二路光伏历史最高功率 (W) |
| `pv_power_total` | float | 光伏输入总功率 (W) |
| `mppt_state` | string | MPPT 状态：`tracking`、`standby`、`float`、`off` |

#### data/status（逆变器系统状态）

频率：5s

```json
{
  "state": "inverting",
  "fault_code": 0,
  "alarm_code": 0,
  "temp_inv": 48.5,
  "temp_mos": 55.2,
  "temp_ambient": 32.0,
  "dc_bus_voltage": 380.0,
  "efficiency": 94.6,
  "runtime_hours": 8640,
  "fan_speed": 60
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `state` | string | 运行状态：`inverting`=逆变运行、`standby`=待机、`fault`=故障、`shutdown`=关机、`bypass`=旁路 |
| `fault_code` | int | 故障码，0 表示无故障 |
| `alarm_code` | int | 告警码，0 表示无告警 |
| `temp_inv` | float | 逆变器内部温度 (℃) |
| `temp_mos` | float | 散热器温度 (℃) |
| `temp_ambient` | float | 环境温度 (℃)，可选 |
| `dc_bus_voltage` | float | 直流母线电压 (V) |
| `efficiency` | float | 逆变效率 (%) |
| `runtime_hours` | int | 累计运行时长 (小时) |
| `fan_speed` | int | 风扇转速 (%)，0=停转 |

#### data/energy（能量统计）

频率：60s

```json
{
  "daily_pv": 8.56,
  "total_pv": 1250.3,
  "daily_charge": 7.80,
  "total_charge": 1100.5,
  "daily_discharge": 6.20,
  "total_discharge": 980.8,
  "daily_load": 6.00,
  "total_load": 950.2,
  "runtime_hours": 8640
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `daily_pv` | float | 当日光伏发电量 (kWh) |
| `total_pv` | float | 累计光伏发电量 (kWh) |
| `daily_charge` | float | 当日充入电池电量 (kWh) |
| `total_charge` | float | 累计充电量 (kWh) |
| `daily_discharge` | float | 当日电池放电量 (kWh) |
| `total_discharge` | float | 累计放电量 (kWh) |
| `daily_load` | float | 当日负载消耗 (kWh) |
| `total_load` | float | 累计负载消耗 (kWh) |
| `runtime_hours` | int | 累计运行时长 (小时) |

#### data/cells（电芯详情）

频率：30s

```json
{
  "cell_count": 16,
  "voltages": [
    3.32, 3.33, 3.31, 3.32, 3.35, 3.30, 3.32, 3.31,
    3.33, 3.32, 3.31, 3.32, 3.34, 3.29, 3.32, 3.31
  ],
  "temps": [
    26.5, 26.8, 26.2, 26.5, 27.0, 26.0, 26.5, 26.3,
    26.8, 26.5, 26.2, 26.5, 26.9, 25.8, 26.5, 26.4
  ]
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `cell_count` | int | 电芯串数 |
| `voltages` | float[] | 各电芯电压 (V)，数组长度 = `cell_count` |
| `temps` | float[] | 各电芯温度 (℃)，数组长度 = `cell_count` |

#### data/parallel（并机信息）

频率：5s

```json
{
  "mode": "standalone",
  "count": 1,
  "total_rated_power": 6200,
  "total_active_power": 1870.3,
  "sync_state": "synced",
  "machines": []
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `mode` | string | 并机模式：`standalone`、`parallel`、`three_phase` |
| `count` | int | 并机台数 |
| `total_rated_power` | int | 并机总额定功率 (W) |
| `total_active_power` | float | 并机总有功功率 (W) |
| `sync_state` | string | 同步状态：`synced`、`syncing`、`error` |
| `machines` | array | 各机器状态列表 |

#### data/three_phase（三相数据）

频率：5s

```json
{
  "voltage_l1": 220.5,
  "voltage_l2": 220.1,
  "voltage_l3": 220.3,
  "current_l1": 8.52,
  "current_l2": 8.50,
  "current_l3": 8.48,
  "power_l1": 1870.3,
  "power_l2": 1868.5,
  "power_l3": 1865.2,
  "power_total": 5604.0,
  "voltage_ll": 381.8,
  "frequency": 50.02,
  "unbalance_v": 0.5,
  "unbalance_i": 0.8
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `voltage_l1` | float | L1 相电压 (V) |
| `voltage_l2` | float | L2 相电压 (V) |
| `voltage_l3` | float | L3 相电压 (V) |
| `current_l1` | float | L1 相电流 (A) |
| `current_l2` | float | L2 相电流 (A) |
| `current_l3` | float | L3 相电流 (A) |
| `power_l1` | float | L1 相有功功率 (W) |
| `power_l2` | float | L2 相有功功率 (W) |
| `power_l3` | float | L3 相有功功率 (W) |
| `power_total` | float | 三相总有功功率 (W) |
| `voltage_ll` | float | 线电压 (V) |
| `frequency` | float | 输出频率 (Hz) |
| `unbalance_v` | float | 电压不平衡度 (%) |
| `unbalance_i` | float | 电流不平衡度 (%) |

#### data/control（远程控制状态）

频率：5s

```json
{
  "power_limit": 6200,
  "charge_enable": true,
  "discharge_enable": true,
  "grid_charge_enable": false,
  "max_charge_current": 30.0,
  "max_discharge_current": 40.0
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `power_limit` | int | 有功功率上限 (W) |
| `charge_enable` | bool | 充电使能 |
| `discharge_enable` | bool | 放电使能 |
| `grid_charge_enable` | bool | 电网充电使能 |
| `max_charge_current` | float | 最大充电电流 (A) |
| `max_discharge_current` | float | 最大放电电流 (A) |

#### data/alarm（告警/故障事件）

频率：事件触发，QoS 1

```json
{
  "code": 1001,
  "level": "warning",
  "message": "电池电压过低",
  "timestamp": 1716800000
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `code` | int | 告警/故障码 |
| `level` | string | 级别：`info`、`warning`、`fault` |
| `message` | string | 告警/故障描述 |
| `timestamp` | int | 事件发生时间（Unix 时间戳，秒） |

**告警码定义**：

| 代码 | 级别 | 名称 | 说明 | 恢复条件 |
|------|------|------|------|----------|
| 1 | fault | 逆变器过温保护 | 温度 >85℃ | 温度降至 75℃以下 |
| 2 | fault | 电池过压保护 | 电池电压超限 | 电压恢复后手动复位 |
| 3 | fault | 电池欠压保护 | 电池电压过低 | 电压恢复正常 |
| 4 | fault | 输出过载保护 | 功率 >6200W | 负载降低后手动复位 |
| 5 | fault | 直流母线过压 | 直流母线电压超限 | 电压恢复后手动复位 |
| 6 | warning | 逆变器温度过高 | 温度 >75℃ | 温度降至 70℃以下 |
| 7 | warning | 电池SOC过低 | SOC <10% | SOC 恢复至 15%以上 |
| 8 | warning | PV输入异常 | 光伏输入电压/电流异常 | PV 恢复正常 |
| 9 | warning | 电芯压差过大 | 单体压差 >100mV | 压差恢复至 50mV以下 |
| 10 | info | 系统启动完成 | 上电初始化完成 | - |
| 11 | info | 进入待机模式 | 系统进入待机 | - |
| 12 | info | 恢复并网运行 | 系统恢复正常运行 | - |

#### ota/status（OTA 升级状态）

频率：按需，QoS 1

```json
{
  "target": "esp",
  "state": "downloading",
  "progress": 45,
  "message": "下载中..."
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `target` | string | 升级目标：`esp` 或 `arm` |
| `state` | string | 状态：`idle`、`downloading`、`uploading`、`verifying`、`done`、`error` |
| `progress` | int | 进度 0-100 |
| `message` | string | 状态消息 |

---

## 六、数据下行（云端 → 设备）

### 6.1 命令下发

云端通过 `cs_inv/{sn}/cmd` 下发命令，ESP32 透传到 ARM：

**云端 → ESP32 MQTT**：

```
主题: cs_inv/H1CNA00135000014/cmd
Payload: {"topic":"ac_on","payload":""}
```

**ESP32 → ARM UART**：

```
CMD=0x03 (COMMAND_RECV)
DATA={"topic":"ac_on","payload":""}
```

### 6.2 支持的控制命令

#### 逆变器控制

| 命令 | Payload | 说明 |
|------|---------|------|
| `ac_on` | `""` | 交流输出开启（需先使能远程控制） |
| `ac_off` | `""` | 交流输出关闭 |
| `set_power_limit` | `{"value":3000}` | 有功功率上限 (W) |
| `set_reactive` | `{"value":500}` | 无功功率目标值 (Var) |
| `set_pf` | `{"value":99}` | 目标功率因数（-100~100，对应 -1.00~1.00） |
| `set_charge_limit` | `{"value":3000}` | 最大充电功率 (W) |
| `set_discharge_limit` | `{"value":3000}` | 最大放电功率 (W) |
| `set_soc_low` | `{"value":200}` | 放电截止 SOC（0.1%，200=20%） |
| `set_soc_high` | `{"value":950}` | 充电截止 SOC（0.1%，950=95%） |
| `force_charge` | `{"value":1}` | 强制充电使能：0=关闭, 1=开启 |
| `force_discharge` | `{"value":1}` | 强制放电使能：0=关闭, 1=开启 |
| `grid_charge_enable` | `{"value":1}` | 电网充电使能：0=关闭, 1=开启 |
| `eco_mode` | `{"value":1}` | 工作模式：0=自发自用, 1=备电优先, 2=分时电价, 3=离网 |
| `restart` | `""` | 故障复位 |
| `pv_shutdown` | `{"value":3}` | 组串关断（Bit0=PV1, Bit1=PV2） |
| `query` | `""` | 立即上报全量数据 |

#### BMS 控制

| 命令 | Payload | 说明 |
|------|---------|------|
| `bms/charge_enable` | `{"value":1}` | 充放电使能：0=禁止, 1=允许 |
| `bms/charge_current` | `{"value":500}` | 最大充电电流（0.1A，500=50A） |
| `bms/discharge_current` | `{"value":1000}` | 最大放电电流（0.1A，1000=100A） |
| `bms/charge_volt` | `{"value":584}` | 充电截止电压（0.1V，584=58.4V） |
| `bms/discharge_volt` | `{"value":430}` | 放电截止电压（0.1V，430=43.0V） |
| `bms/balance_enable` | `{"value":1}` | 均衡使能：0=禁止, 1=允许 |
| `bms/balance_threshold` | `{"value":50}` | 均衡启动压差 (mV) |

#### MPPT 控制

| 命令 | Payload | 说明 |
|------|---------|------|
| `mppt_on` | `""` | 启用 MPPT 充电 |
| `mppt_off` | `""` | 禁用 MPPT 充电 |
| `mppt_power_limit` | `{"value":1000}` | PV 输入功率限制 (W) |

#### EPS 应急电源控制

| 命令 | Payload | 说明 |
|------|---------|------|
| `eps_enable` | `{"value":1}` | 启用 EPS：0=禁用, 1=启用 |
| `eps_power_limit` | `{"value":3000}` | EPS 最大输出功率 (W) |
| `eps_voltage_set` | `{"value":220}` | EPS 输出电压设定 (V) |

#### 发电机控制

| 命令 | Payload | 说明 |
|------|---------|------|
| `gen_enable` | `{"value":1}` | 启用发电机：0=禁用, 1=启用 |
| `gen_power_limit` | `{"value":5000}` | 发电机最大输出功率 (W) |

### 6.3 OTA 远程升级

云端通过 `cs_inv/{sn}/ota/cmd` 下发 OTA 命令：

**OTA 命令格式**：

```json
{
  "action": "start",
  "target": "esp",
  "url": "http://firmware.example.com/esp32-v1.0.6.bin"
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `action` | string | 是 | 操作：`start`、`stop`、`query` |
| `target` | string | 是 | 目标：`esp`（ESP32/ESP8266）、`arm`（ARM芯片） |
| `url` | string | start 时必填 | 固件下载 URL（HTTP） |

### 6.4 命令回复

ARM 执行后通过 `CMD=0x04` 回复，ESP32 转发到 `cs_inv/{sn}/cmd/response`。

---

## 七、心跳与在线状态

### 7.1 ESP32 → ARM 心跳

```
CMD=0x06 (HEARTBEAT)
DATA=(空)
```

发送间隔：10s，表示 ESP32 在线。ARM 超时 30s 未收到心跳则报警。

### 7.2 ESP32 → 云端 status 心跳

ESP32 每 60s 自动发布到 `cs_inv/{sn}/status`（QoS 1, Retain true）：

```json
{
  "online": true,
  "rssi": -45,
  "ip": "192.168.1.100"
}
```

### 7.3 LWT 遗嘱

MQTT 连接时设置 LWT，设备异常断开时自动发布：

```
主题: cs_inv/{sn}/status
Payload: {"online":false}
Retain: true
```

后端通过监听 `cs_inv/+/status` 的 retained 消息判断设备在线/离线。
