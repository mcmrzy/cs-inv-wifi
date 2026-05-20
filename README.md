# CS-I10-6k2 单相离网48V逆变器 WiFi 通信模块

辰烁科技 CS-I10-6k2 48V 单相离网逆变器的 ESP32-C3 WiFi 通信固件。

## 产品规格

| 参数 | 值 |
|------|-----|
| 型号 | CS-I10-6k2 |
| 类型 | 单相离网逆变器 |
| 额定功率 | 6200W |
| 额定电压 | 220V / 50Hz |
| 电池电压 | 48V (16S LiFePO4) |
| MPPT 电压范围 | 60~150V |
| 通信模块 | ESP32-C3 |

## 项目结构

```
arduino_inverter_wifi/
├── src/                    # ESP32 固件源码
│   ├── main.cpp            # 主程序入口
│   ├── config.h            # 配置参数
│   ├── mqtt_handler.cpp/h  # MQTT 通信处理
│   ├── uart_protocol.cpp/h # UART 帧协议
│   ├── nvs_storage.cpp/h   # NVS 存储管理
│   ├── led_status.cpp/h   # LED 状态指示
│   ├── tcp_provision.cpp/h# TCP 配网
│   ├── sn_encoder.cpp/h    # SN 编码/校验
│   ├── ip_geolocation.cpp/h# IP 地理位置
│   └── mqtt_ca_cert.h     # MQTT CA 证书
├── tools/                  # 开发工具
│   ├── arm_inverter_simulator_gui.py  # ARM 模拟器 GUI
│   ├── esp32_gateway_simulator.py      # ESP32 网关模拟器
│   └── ...
├── docs/                   # 协议规范文档
│   ├── 系统参数规范.md     # MQTT 数据参数定义
│   ├── MQTT通信协议.md     # MQTT 通信协议
│   └── ARM与ESP32通信协议.md # UART 帧协议
└── .pio/                  # PlatformIO 构建目录
```

## 功能特性

### 通信功能
- **UART 透传**：ARM MCU 与 ESP32 之间通过二进制帧协议通信
- **MQTT 上报**：支持 8 个数据主题上报（AC/电池/光伏/状态/能量/电芯/告警/设备信息）
- **云端控制**：支持 19 种控制命令（逆变器/BMS/MPPT）
- **IP 定位**：WiFi 连接后自动获取设备地理位置
- **LWT 遗嘱**：设备离线自动通知

### 数据上报
| 主题 | 频率 | 说明 |
|------|------|------|
| `cs_inv/{sn}/info` | 连接时 | 设备信息 |
| `cs_inv/{sn}/data/ac` | 5s | 交流输出参数 |
| `cs_inv/{sn}/data/battery` | 5s | 电池 BMS 参数 |
| `cs_inv/{sn}/data/pv` | 5s | 光伏 MPPT 参数 |
| `cs_inv/{sn}/data/status` | 5s | 逆变器系统状态 |
| `cs_inv/{sn}/data/energy` | 60s | 能量统计 |
| `cs_inv/{sn}/data/cells` | 30s | 电芯详细数据 |
| `cs_inv/{sn}/data/alarm` | 事件 | 告警/故障事件 |

### 控制命令
- **逆变器控制**：开关机、电压/频率设定、功率限制、节能模式
- **BMS 控制**：充放电使能、电流/电压限值、均衡控制
- **MPPT 控制**：开关控制、功率限制

## 固件编译

### 环境要求
- PlatformIO Core 或 PlatformIO IDE
- ESP32 开发板支持

### 编译步骤
```bash
# 克隆项目
git clone <repository_url>
cd arduino_inverter_wifi

# 安装依赖
pio lib install

# 编译
pio run

# 上传固件
pio run --target upload
```

### 串口监视
```bash
pio device monitor
```

## 开发工具

### ARM 模拟器
用于在没有实际 ARM MCU 的情况下测试 ESP32 固件：

```bash
cd tools
python arm_inverter_simulator_gui.py
```

### ESP32 网关模拟器
用于在没有实际 ESP32 的情况下测试 MQTT 云端对接：

```bash
cd tools
python esp32_gateway_simulator.py
```

## 协议文档

详细协议规范请参考 `docs/` 目录：

- **系统参数规范.md** — MQTT 数据字段定义、JSON 格式、数据校验范围
- **MQTT通信协议.md** — MQTT 连接参数、主题格式、上下行数据格式
- **ARM与ESP32通信协议.md** — UART 帧协议格式、命令码定义

## 硬件连接

```
ARM MCU          ESP32-C3
-------          ---------
TX  ────────────► RX (GPIO1)
RX ◄──────────── TX (GPIO0)
GND ──────────── GND
```

| 参数 | 值 |
|------|-----|
| 波特率 | 115200 |
| 数据位 | 8 |
| 停止位 | 1 |
| 校验 | 无 |

## 配置说明

### 默认 MQTT Broker
- 地址：`jiuxiaoyw.online:8883`
- 用户名：`CSKJ-INV-DEVICE-6K2`
- 密码：`CSKJINVDEVICE6K2`

### SN 格式
设备序列号遵循 CS-SN-STD-001 V1.1 标准：
```
MM-CC-PPPP-YW-SSSSS-C
│  │  │    │  │     │
│  │  │    │  │     └── 校验位 (CRC-16-CCITT)
│  │  │    │  └───────── 序列号 (5位数字)
│  │  │    └──────────── 年月码 (年码1位 + 月码1位)
│  │  └───────────────── 产品型号 (4位)
│  └───────────────────── 客户代码 (2位)
└───────────────────────── 制造商代码 (2位)
```

## License

Copyright © 辰烁科技
