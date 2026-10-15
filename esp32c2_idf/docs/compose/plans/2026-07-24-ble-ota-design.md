# BLE OTA 设计方案

## 目标

通过 BLE GATT 实现本地 OTA 升级，替代 AP + HTTP 方案。不开 SoftAP、不开 HTTP Server，节省 ESP32-C2 内存。

## 架构概览

```
手机 APP (nRF Connect / 自定义APP)
    │
    │  BLE GATT (NimBLE)
    ▼
┌─────────────────────────────┐
│  ble_ota GATT Service       │
│  ├─ CTRL  (Write)           │  ← 控制命令：开始/取消
│  ├─ DATA  (Write+Notify)    │  ← 固件数据写入 + ACK
│  └─ STATUS (Read+Notify)    │  ← 进度/状态回报
└─────────┬───────────────────┘
          │ OtaStreamReader
          ▼
┌─────────────────────────────┐
│  ota_service_start_local()  │  ← 复用现有 OTA 框架
│  ├─ 签名验证                │
│  ├─ SHA256 校验             │
│  └─ esp_ota_write()         │
└─────────────────────────────┘
```

## BLE GATT 服务定义

### Service UUID
`43534f54-4100-1000-8000-00805f9b34fb` (CSOT)

### Characteristics

| Characteristic | UUID | 属性 | 说明 |
|---------------|------|------|------|
| OTA_CTRL | `43534f54-4354-...` | Write | 控制命令 |
| OTA_DATA | `43534f54-4441-...` | Write + Notify | 固件数据 / ACK |
| OTA_STATUS | `43534f54-5354-...` | Read + Notify | 进度状态 |

### OTA_CTRL 命令格式

```
[1B cmd] [payload...]

cmd=0x01 START:
  [1B target] [4B file_size LE] [32B sha256] [version_str...]
  target: 0=ESP, 1=ARM

cmd=0x02 CANCEL: 无 payload

cmd=0x03 READY_ACK: 设备收到 START 后回复，手机确认开始传数据
```

### OTA_DATA 数据格式

```
[2B seq LE] [payload]

seq: 从 0 递增的包序号
payload: 固件数据块，最大 MTU-3 字节
```

设备收到后通过 Notify 回复 ACK:

```
[1B status] [2B seq LE]
status: 0=OK, 1=CRC_ERR, 2=SEQ_ERR, 3=BUSY
```

### OTA_STATUS 状态格式

```
[1B state] [1B progress] [4B received LE] [4B total LE]
[state_str...]
```

state 值: 0=idle, 1=ready, 2=receiving, 3=verifying, 4=done, 5=error

## MTU 优化

- 启动时请求 MTU 247（BLE 5.0 DLE）
- 有效 payload per packet = MTU - 3 (ATT header) - 2 (seq) = 242 bytes
- 实际吞吐: ~15-25 KB/s（连接间隔 7.5ms, 每 connection event 多包）
- 500KB 固件 ≈ 20-35 秒

## 实现计划

### 新增文件

| 文件 | 说明 |
|------|------|
| `main/ble_ota.h` | BLE OTA 公共接口 |
| `main/ble_ota.c` | BLE OTA GATT 服务 + 协议实现 |

### 修改文件

| 文件 | 修改内容 |
|------|---------|
| `main/app_main.c` | 初始化 ble_ota，注册到 BLE |
| `components/ble_prov/ble_prov.c` | 允许在已初始化的 BLE 栈上添加新 service |
| `main/CMakeLists.txt` | 添加 ble_ota.c |

### ble_ota.h 接口

```c
bool ble_ota_init(void);
bool ble_ota_is_active(void);
void ble_ota_cancel(void);
```

### ble_ota.c 核心逻辑

1. `ble_ota_init()`: 注册 GATT service（在 ble_prov_start 之后调用，共享 NimBLE 栈）
2. CTRL 写入处理: 解析 START 命令，构造 OtaManifest，调用 `ota_service_start_local()`
3. DATA 写入处理: 将收到的数据块缓存到 ring buffer，供 OtaStreamReader 读取
4. 状态通知: 定期将 OTA 进度通过 STATUS characteristic notify 出去

### 数据流

```
BLE DATA Write → ring_buffer (2KB) → OtaStreamReader → ota_service → esp_ota_write
                  ↑ 信号量同步          ↑ 阻塞等待数据
```

使用 2KB ring buffer 做生产者-消费者模型:
- BLE 回调写入 ring buffer（生产者）
- OTA StreamReader 从中读取（消费者）
- 信号量控制流控，避免溢出

## 内存开销

| 项目 | 大小 |
|------|------|
| Ring buffer | 2 KB |
| GATT attribute table | ~200 B |
| 状态/控制变量 | ~200 B |
| **总计** | **~2.5 KB** |

不开 AP + HTTP 可省约 15-20 KB，净省 ~17 KB。

## 手机端

- 开发阶段: nRF Connect APP 手动写入 CTRL/DATA characteristic
- 生产阶段: 自定义 APP（Android/iOS）或小程序

## 待确认

1. BLE 和 WiFi 能否同时运行？→ ESP32-C2 支持共存，但需确认内存够用
2. OTA 期间是否停止 BLE 配网广播？→ 建议停止，OTA 完成后恢复
3. 断点续传？→ V1 不支持，断开后重新开始
