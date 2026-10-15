# ESP32-C2 2MB Flash 优化方案

## 空间分析

### Arduino ESP32-C3 程序空间占用

| 模块 | 文件 | 大小 | 说明 |
|------|------|------|------|
| HTTP配网 | tcp_provision.cpp | 56KB | 含HTML网页(~15KB) |
| 主程序 | main.cpp | 29KB | 状态机、WiFi、MQTT |
| MQTT OTA | mqtt_ota_handler.cpp | 26KB | MQTT远程升级 |
| OTA管理 | ota_manager.cpp | 23KB | OTA通用逻辑 |
| ARM OTA | arm_ota.cpp | 22KB | ARM固件升级 |
| MQTT处理 | mqtt_handler.cpp | 12KB | 消息收发 |
| SN编码 | sn_encoder.cpp | 11KB | 序列号处理 |
| 离线缓存 | offline_cache.cpp | 10KB | 断网数据缓存 |
| **总计** | | **~190KB** | 源代码 |

### ESP-IDF 2MB Flash 可用空间

| 分区 | 大小 | 说明 |
|------|------|------|
| bootloader | 20KB | 启动程序 |
| nvs | 16KB | NVS存储 |
| phy_init | 4KB | PHY初始化 |
| otadata | 8KB | OTA数据 |
| **ota_0** | **1920KB** | 应用程序 |
| **总计** | **~1968KB** | |

**实际可用**: ~1920KB - 代码段 - 数据段 ≈ **600-800KB 可用**

---

## 优化方案

### 1. 移除 ARM OTA 功能 (节省 ~22KB)

**原因**: ARM OTA 需要通过 ESP32 升级 ARM MCU 固件，对于2MB版本可以移除

**修改**:
- 删除 `arm_ota.c` 和 `arm_ota.h`
- 从 CMakeLists.txt 移除
- 保留 CMD_ARM_* 命令定义（兼容性）

### 2. 精简 MQTT OTA (节省 ~15KB)

**原因**: 完整OTA功能较大，可以简化为基本功能

**修改**:
- 简化 `mqtt_ota_handler.c`
- 移除断点续传、进度显示等高级功能
- 保留基本的OTA下载和写入

### 3. 精简 HTTP 配网页面 (节省 ~10KB)

**原因**: HTML/CSS/JS 占用较大空间

**修改**:
- 使用更精简的HTML
- 移除WiFi扫描功能（用SmartConfig代替）
- 压缩CSS和JS

### 4. 减少缓冲区大小 (节省 ~15KB RAM)

**修改**:
```c
// 原配置
#define FRAME_DATA_MAX   1024
#define PUB_QUEUE_SIZE   16
#define PUB_PAYLOAD_LEN  600

// 精简配置
#define FRAME_DATA_MAX   512
#define PUB_QUEUE_SIZE   4
#define PUB_PAYLOAD_LEN  300
```

### 5. 移除调试日志 (节省 ~5KB Flash)

**修改**:
```c
// sdkconfig
CONFIG_LOG_DEFAULT_LEVEL_NONE=y
```

### 6. 优化离线缓存 (节省 ~3KB RAM)

**修改**:
```c
#define OFFLINE_CACHE_MAX_ITEMS  10  // 原来是 CONFIG 值
```

---

## 优化后空间估算

| 优化项 | 节省Flash | 节省RAM |
|--------|----------|---------|
| 移除ARM OTA | 22KB | 2KB |
| 精简MQTT OTA | 15KB | 1KB |
| 精简HTTP页面 | 10KB | 0KB |
| 减少缓冲区 | 0KB | 15KB |
| 移除调试日志 | 5KB | 0KB |
| 优化离线缓存 | 0KB | 3KB |
| **总计** | **~52KB** | **~21KB** |

**优化后**: ~190KB - 52KB = **~138KB 源代码**

编译后二进制: ~138KB × 1.5 ≈ **207KB**

**剩余空间**: 600KB - 207KB ≈ **393KB 可用**

---

## 移植步骤

### 1. 使用精简版配置

```bash
cp main/config_lite.h main/config.h
cp main/CMakeLists_lite.txt main/CMakeLists.txt
```

### 2. 删除不必要的文件

```bash
rm main/arm_ota.c main/arm_ota.h
rm main/mqtt_ota_handler.c main/mqtt_ota_handler.h
```

### 3. 简化 tcp_provision.c

- 移除 HTML 网页的 WiFi 扫描功能
- 使用更精简的 HTML 模板
- 减少 HTTP 客户端槽位

### 4. 修改 sdkconfig

```ini
# 精简日志
CONFIG_LOG_DEFAULT_LEVEL_NONE=y

# 精简缓冲区
CONFIG_MQTT_BUFFER_SIZE=512
CONFIG_MQTT_TASK_STACK_SIZE=2048

# 精简分区表
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_2mb.csv"
```

### 5. 重新编译

```bash
idf.py build
```

---

## 功能保留清单

### 保留功能
- [x] WiFi STA 连接
- [x] WiFi AP 热点
- [x] SmartConfig 配网
- [x] MQTT 连接和数据发布
- [x] UART 协议通信
- [x] LED 状态指示
- [x] 按钮重置
- [x] NVS 存储
- [x] SN 编码
- [x] 基本 OTA (可选)

### 移除功能
- [ ] ARM OTA
- [ ] MQTT OTA (高级功能)
- [ ] WiFi 扫描
- [ ] 离线缓存 (可选保留)
- [ ] 详细调试日志

---

## 注意事项

1. **测试**: 优化后需要充分测试所有功能
2. **兼容性**: 保持 UART 协议兼容
3. **OTA**: 如果需要 OTA，保留基本功能即可
4. **回退**: 保留原始代码以便回退
