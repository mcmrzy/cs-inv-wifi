# MQTT OTA 功能集成说明

## 📋 功能概述

ESP32 现已支持通过 MQTT 远程升级 ESP 和 ARM 固件。

### 工作流程

```
远程管理后台
    │
    ├─ 上传固件到服务器
    │
    ├─ 通过 MQTT 发送 OTA 命令
    │  主题: cs_inv/{sn}/ota/cmd
    │  格式: {"command":"start", "target":"esp", "url":"http://...", ...}
    │
    ▼
ESP32 接收命令
    │
    ├─ 解析 JSON
    │
    ├─ HTTP 流式下载固件
    │  同时上报进度到 MQTT
    │
    ├─ ESP: Update.write() 写入 Flash
    │  ARM: arm_ota_feed() 转发给 ARM
    │
    ├─ 校验并重启
    │
    ▼
设备运行新固件
    │
    └─ 上报版本信息到 MQTT
```

---

## 🚀 使用方法

### 1. 后台下发 OTA 命令

**MQTT 主题**：`cs_inv/{设备SN}/ota/cmd`

**命令格式**（JSON）：

```json
{
  "command": "start",
  "target": "esp",
  "firmware_id": "fw_esp_1.2.0_abc123",
  "url": "https://firmware.example.com/esp_1.2.0_abc123.bin",
  "version": "1.2.0",
  "md5": "a1b2c3d4e5f6g7h8i9j0k1l2m3n4o5p6",
  "file_size": 1048576,
  "timeout": 300,
  "task_id": "task_20240601_001",
  "timestamp": 1717200000
}
```

**参数说明**：

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| command | string | 是 | 操作：`start`、`stop`、`query` |
| target | string | 是 | 目标：`esp` 或 `arm` |
| firmware_id | string | 否 | 固件 ID（用于追踪） |
| url | string | 是 | 固件下载 URL（HTTP/HTTPS） |
| version | string | 是 | 目标版本号 |
| md5 | string | 否 | MD5 校验值（可选） |
| file_size | int | 否 | 固件大小（字节） |
| timeout | int | 否 | 超时时间（秒），默认 300 |
| task_id | string | 否 | 任务 ID（关联日志） |
| timestamp | int | 否 | 时间戳 |

### 2. 设备上报进度

**主题**：`cs_inv/{sn}/ota/status`  
**频率**：每 1-5 秒上报一次  
**QoS**：0

**响应格式**：

```json
{
  "task_id": "task_20240601_001",
  "target": "esp",
  "state": "downloading",
  "progress": 45,
  "speed_kbps": 1024,
  "remaining_seconds": 10,
  "detail": "下载中: 45%",
  "timestamp": 1717200045
}
```

**状态值**：
- `preparing` - 准备中
- `downloading` - 下载中
- `writing` - 写入中
- `verifying` - 校验中
- `done` - 完成
- `cancelled` - 已取消
- `error` - 错误

### 3. 错误上报

**主题**：`cs_inv/{sn}/ota/error`  
**QoS**：1

```json
{
  "task_id": "task_20240601_001",
  "target": "esp",
  "error_code": "DOWNLOAD_TIMEOUT",
  "error_message": "下载超时（300s）",
  "timestamp": 1717200100
}
```

---

## 📝 常见错误码

| 错误码 | 说明 |
|--------|------|
| INVALID_PARAMS | 参数不完整或格式错误 |
| INVALID_TARGET | target 只能是 esp 或 arm |
| OTA_BUSY | 已有 OTA 操作进行中 |
| HTTP_INIT_FAILED | HTTP 客户端初始化失败 |
| HTTP_ERROR | HTTP 请求失败 |
| INVALID_SIZE | 文件大小无效 |
| FILE_TOO_LARGE | 文件过大（ESP >1MB, ARM >256KB） |
| DOWNLOAD_TIMEOUT | 下载超时（300s） |
| WRITE_FAILED | Flash 写入失败 |
| VERIFY_FAILED | 固件校验失败 |
| UPDATE_BEGIN_FAILED | Update 初始化失败 |
| ARM_OTA_START | ARM OTA 启动失败 |
| ARM_FEED_FAILED | ARM 缓冲失败 |
| ARM_END_FAILED | ARM 结束失败 |

---

## 🔧 代码集成说明

### 新建文件

1. **mqtt_ota_handler.h** - MQTT OTA 处理头文件
2. **mqtt_ota_handler.cpp** - MQTT OTA 处理实现

### 修改文件

1. **main.cpp**
   - 添加 `#include "mqtt_ota_handler.h"`
   - 在 `setup()` 中调用 `mqtt_ota_init()`
   - 在 `loop()` 的 `STATE_RUNNING` 中调用 `mqtt_ota_process()`
   - 修改 `on_mqtt_message()` 中的 OTA 命令处理

### 关键函数

**mqtt_ota_handler.h 中的 API**：

```cpp
// 初始化
void mqtt_ota_init();

// 处理 MQTT 命令
void mqtt_ota_handle_command(const char* topic, const uint8_t* payload, unsigned int len);

// 循环处理（在 loop() 中调用）
void mqtt_ota_process();

// 状态查询
bool mqtt_ota_is_busy();
const char* mqtt_ota_get_task_id();
```

---

## 📊 工作方式细节

### ESP OTA 流程

```
接收命令
  │
  ├─ 验证参数
  │
  ├─ 创建 HTTP 连接
  │
  ├─ 获取文件大小
  │
  ├─ Update.begin(size)
  │
  ├─ 流式下载
  │  ├─ 每 4KB 读取一次
  │  ├─ Update.write() 写入 Flash
  │  ├─ 每秒上报进度
  │  └─ 处理 UART 防止超时
  │
  ├─ 校验完整性
  │  ├─ Update.end(true)
  │  ├─ 自动重启
  │  └─ 新固件运行
  │
  └─ 完成
```

### ARM OTA 流程

```
接收命令
  │
  ├─ arm_ota_start_streaming(size)
  │
  ├─ 流式下载
  │  ├─ 每 1KB 读取一次
  │  ├─ arm_ota_feed() 缓冲
  │  ├─ 自动分包发送给 ARM
  │  └─ 处理 UART ACK/NACK
  │
  ├─ arm_ota_end_streaming()
  │
  ├─ 等待 ARM 校验
  │  ├─ ARM 通过 UART 0x13 ACK 响应
  │  └─ 校验通过后完成
  │
  └─ 完成
```

---

## ⚙️ 配置和限制

### 文件大小限制

- **ESP32**：最大 1MB
- **ARM**：最大 256KB

### 超时时间

- **HTTP 连接**：10 秒
- **HTTP 下载**：取决于 command 中的 timeout 字段（默认 300s）
- **单个 UART 包**：5 秒
- **ARM OTA 总超时**：600 秒

### 缓冲区大小

- **ESP OTA**：4KB 缓冲
- **ARM OTA**：1KB 缓冲（8KB 缓冲区总量）

### 下载速度

支持流式下载，实时上报速度和剩余时间。

---

## 🧪 测试

### 测试命令（MQTT CLI）

```bash
# 订阅状态
mosquitto_sub -h broker.example.com -t 'cs_inv/+/ota/status'

# 发送 ESP OTA 命令
mosquitto_pub -h broker.example.com \
  -t 'cs_inv/H1CNA00135000014/ota/cmd' \
  -m '{
    "command":"start",
    "target":"esp",
    "url":"https://firmware.example.com/esp_1.2.0.bin",
    "version":"1.2.0",
    "task_id":"test_20240601_001"
  }'
```

### 监控日志

```
[MQTT-OTA] 收到命令 (主题: cs_inv/H1CNA00135000014/ota/cmd, 长度: 156)
[MQTT-OTA] Payload: {"command":"start",...}
[MQTT-OTA] 命令: start, 目标: esp, URL: https://firmware.example.com/esp_1.2.0.bin
[MQTT-OTA] 处理 START 命令
[MQTT-OTA] 发送 ACK: {"ack":true,"task_id":"test_20240601_001",...}
[MQTT-OTA-ESP] 开始流式下载和写入
[MQTT-OTA] 上报状态: {"task_id":"test_20240601_001","target":"esp",...}
[MQTT-OTA-ESP] 进度: 50% (524288/1048576), 速度: 1024 KB/s
[MQTT-OTA-ESP] 下载完成, 共 1048576 字节
[MQTT-OTA-ESP] 升级成功，设备即将重启
```

---

## 🔐 安全建议

1. **验证固件**
   - 始终提供 MD5 或 SHA256 校验值
   - 后台验证固件签名

2. **MQTT 安全**
   - 使用 TLS 加密（mqtts://）
   - 设置用户名和密码
   - 限制订阅权限

3. **URL 安全**
   - 仅接受 HTTPS 协议
   - 验证 SSL 证书
   - 限制允许的域名

4. **版本管理**
   - 检查版本号防止降级
   - 记录所有升级历史
   - 支持紧急回滚

---

## 🐛 故障排查

### 问题：连接超时

**原因**：网络不稳定或服务器无响应  
**解决**：检查固件 URL 是否可访问，增加 timeout 值

### 问题：MD5 不匹配

**原因**：固件传输过程中被篡改  
**解决**：重试下载，检查网络质量

### 问题：ARM 未响应

**原因**：UART 通信失败或 ARM 固件有问题  
**解决**：检查 UART 连接，重启设备，查看 ARM 日志

### 问题：Flash 写入失败

**原因**：Flash 损坏或内存不足  
**解决**：重启设备后重试，如多次失败可能是硬件问题

---

## 📚 相关文档

- `MQTT接口文档.md` - MQTT 协议完整说明
- `OTA_Design.md` - OTA 系统设计文档
- `ARM_ESP32_UART_Protocol.md` - UART 协议说明

---

## 📞 支持

如有问题或建议，请查看日志输出或提交 issue。
