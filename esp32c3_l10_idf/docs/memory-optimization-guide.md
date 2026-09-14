# ESP32-C3 内存优化指南 (CS-L10-6K2)

> **目的**: 记录当前固件的内存占用分析和可选优化措施，当需要增加 2.8 寸 SPI 屏幕等功能且内存不足时参考。
>
> **生成时间**: 2026-07 | **IDF 版本**: ESP-IDF 5.5.4 | **芯片**: ESP32-C3 (400KB SRAM, 无 PSRAM)

---

## 1. 当前内存概况

### 1.1 SRAM 总量

ESP32-C3 拥有 **400KB SRAM**，IRAM 和 DRAM 映射到同一物理区域（约 322KB 可用）。

### 1.2 Linker Map 关键地址

| 符号 | 地址 | 说明 |
|------|------|------|
| `dram0_0_seg` 起始 | `0x3FC80000` | DRAM 段起始 (0x3FC7C000 + 0x4000 偏移) |
| `_data_start` | `0x3FC98400` | .data 段起始 |
| `_bt_controller_bss_end` | `0x3FCA58A4` | BT controller .bss 结束 |
| `_bss_end` | `0x3FCA58A8` | 全部 .bss 结束 |
| `_heap_start` | `0x3FCA58B0` | 堆区起始 |
| `_heap_end` | `0x40000000` | 堆区结束 |

### 1.3 内存分区

| 区域 | 大小 | 说明 |
|------|------|------|
| .data + .bss (静态分配) | **~154 KB** | 含 BT controller/WiFi/FreeRTOS/应用静态变量 |
| 堆区 (运行时可用) | **~160 KB** | 需扣除 WiFi/BT/MQTT 运行时动态分配 |
| **运行时实际可用堆** | **~80-100 KB** | WiFi+BT+MQTT 运行时额外消耗 60-80KB |

### 1.4 ELF Binary Size (仅供参考，大部分在 Flash)

```
   text     data      bss      dec      hex
1066799   198956  1122748  2388503  0x247217
```

- `text` (~1.04 MB) + `rodata` → Flash (XIP)，不占 SRAM
- `data` (~194 KB) → Flash 加载到 SRAM
- `bss` (~1.1 MB) → SRAM（包含 WiFi/BT 内部大缓冲区）

---

## 2. 当前配置下的内存分配明细

### 2.1 系统任务栈

| 任务 | 栈大小 | 来源 |
|------|--------|------|
| main task | 6144 B | `CONFIG_ESP_MAIN_TASK_STACK_SIZE` |
| system_event | 4096 B | `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE` |
| esp_timer | 3584 B | `CONFIG_ESP_TIMER_TASK_STACK_SIZE` |
| lwIP tcpip | 3072 B | `CONFIG_LWIP_TCPIP_TASK_STACK_SIZE` |
| NimBLE host | 4096 B | `CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE` |
| MQTT task | 8192 B | `CONFIG_MQTT_TASK_STACK_SIZE` |
| **系统任务栈合计** | **~30 KB** | |

### 2.2 应用任务栈

| 任务 | 栈大小 | 文件:行 |
|------|--------|---------|
| `main_loop` | 6144 B | app_main.c:345 |
| `prov_wifi` | 6144 B | app_main.c:152 (临时任务) |
| `arm_uart_rx` | 4096 B | arm_poll.c:445 |
| `ota_health` | 4096 B | ota_service.c:242 |
| `ota_remote` | 6144 B | ota_service.c:754 (按需创建) |
| `ota_reboot` | 2048 B | ota_service.c:851 (临时任务) |
| `cache_flush` | 4096 B | mqtt_handler.c:488 (临时任务) |
| **应用任务栈合计 (常驻)** | **~16 KB** | |

### 2.3 静态缓冲区 (应用层)

| 变量 | 大小 | 文件 |
|------|------|------|
| `g_rx_data_buf` | 1024 B | uart_ota.c:31 |
| `g_stream_buf` | 2048 B | arm_ota.c:49 |
| `s_json_buf` | 1024 B | ota_mqtt_adapter.c:151 |
| `static buf[1800]` | 1800 B | telemetry.c:323 |
| `cs_protocol.buf[1088]` | 1088 B | cs_protocol.h:74 (每个实例) |
| **应用静态缓冲合计** | **~7 KB** | |

### 2.4 WiFi 缓冲区

| 参数 | 当前值 | 单位内存 | 小计 |
|------|--------|---------|------|
| `STATIC_RX_BUFFER_NUM` | 4 | ~1.6 KB | ~6.4 KB |
| `DYNAMIC_RX_BUFFER_NUM` | 8 | ~1.6 KB | ~12.8 KB |
| `DYNAMIC_TX_BUFFER_NUM` | 6 | ~1.6 KB | ~9.6 KB |
| `RX_MGMT_BUF_NUM_DEF` | 5 | ~1.6 KB | ~8 KB |
| WiFi 总计 | | | **~37 KB** |

### 2.5 NimBLE 缓冲区

| 参数 | 当前值 | 说明 |
|------|--------|------|
| `MSYS_1_BLOCK_COUNT` | 8 × 256 B | ~2 KB |
| `MSYS_2_BLOCK_COUNT` | 4 × 320 B | ~1.3 KB |
| `TRANSPORT_ACL_FROM_LL_COUNT` | 8 × 255 B | ~2 KB |
| `TRANSPORT_EVT_COUNT` | 8 × 70 B | ~560 B |
| NimBLE 内部 + 控制器 | | ~15-20 KB |
| NimBLE 总计 | | **~20-25 KB** |

### 2.6 lwIP / mbedTLS / MQTT

| 组件 | 参数 | 值 | 估算 |
|------|------|----|------|
| lwIP | MAX_SOCKETS=10, TCP=16, UDP=16, RAW=16 | | ~8-12 KB |
| lwIP | IPv6 (3 addr, ND6 5 neighbors, 5 prefixes, 3 routers) | | ~5-8 KB |
| lwIP | DHCPS (DHCP Server) | | ~3-5 KB |
| mbedTLS | SSL_IN=4096 + SSL_OUT=2048 | | ~6 KB (运行时) |
| MQTT | BUFFER_SIZE=4096 + 8KB 栈 | | ~12 KB |

---

## 3. 优化方案（按收益排序）

> 以下操作均在 `sdkconfig.defaults` 中添加/修改，修改后需 `idf.py fullclean && idf.py build`。

### 3.1 关闭 IPv6 — 预计节省 ~10-15 KB

设备仅通过 IPv4 MQTT 通信，不需要 IPv6。

```ini
# ---- 关闭 IPv6 (不需要, 节省 ~10-15KB RAM) ----
CONFIG_LWIP_IPV6=n
```

**风险**: 无。设备不对外提供服务，仅 STA 模式连接 WiFi 路由器。

### 3.2 大幅缩减 lwIP 连接数 — 预计节省 ~8-12 KB

设备只需 1 个 MQTT TCP 连接 + DNS 查询，远不需要默认的 16 连接。

```ini
# ---- lwIP 连接数精简 (节省 ~8-12KB RAM) ----
CONFIG_LWIP_MAX_SOCKETS=4            # 10→4
CONFIG_LWIP_MAX_ACTIVE_TCP=4         # 16→4
CONFIG_LWIP_MAX_LISTENING_TCP=2      # 16→2
CONFIG_LWIP_MAX_UDP_PCBS=4           # 16→4
CONFIG_LWIP_MAX_RAW_PCBS=0           # 16→0
CONFIG_LWIP_LOOPBACK_MAX_PBUFS=0     # 8→0
```

**风险**: 低。如后续需要 HTTP Server 或多连接场景，适当调大 `MAX_ACTIVE_TCP`。

### 3.3 关闭 DHCP Server — 预计节省 ~3-5 KB

设备为 STA 模式，不需要内置 DHCP 服务器。

```ini
# ---- 关闭 DHCP Server (STA 模式不需要) ----
CONFIG_LWIP_DHCPS=n
```

**风险**: 无。除非未来要做 SoftAP 热点，届时重新开启。

### 3.4 缩减 MQTT 任务栈和缓冲 — 预计节省 ~4-8 KB

单条 MQTT 消息一般 < 1KB，当前 4KB 缓冲和 8KB 栈过大。

```ini
# ---- MQTT 精简 ----
CONFIG_MQTT_BUFFER_SIZE=2048         # 4096→2048
CONFIG_MQTT_TASK_STACK_SIZE=4096     # 8192→4096
```

**风险**: 低。如果单条 MQTT 消息超过 2KB（如大批量参数上报），可能需要回调缓冲大小。当前 telemetry 最大消息约 1.8KB，2KB 足够。

### 3.5 缩减 NimBLE 配对槽位和特性 — 预计节省 ~5-8 KB

设备只允许 1 个 BLE 连接，不需要记住 8 个配对设备。

```ini
# ---- NimBLE 配对精简 ----
CONFIG_BT_NIMBLE_MAX_BONDS=3         # 8→3
CONFIG_BT_NIMBLE_MAX_CCCDS=3         # 8→3
CONFIG_BT_NIMBLE_WHITELIST_SIZE=3    # 12→3

# ---- 关闭 BLE 5.0 扩展 (配网/本地控制不需要 2M/Coded PHY) ----
CONFIG_BT_NIMBLE_50_FEATURE_SUPPORT=n
```

**风险**: 低。配对记忆从 8 台手机减少到 3 台，足够正常使用。

### 3.6 缩减系统任务栈 — 预计节省 ~5-8 KB

```ini
# ---- 系统任务栈精简 ----
CONFIG_ESP_MAIN_TASK_STACK_SIZE=4096      # 6144→4096
CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=3072  # 4096→3072
CONFIG_ESP_TIMER_TASK_STACK_SIZE=2048     # 3584→2048
CONFIG_LWIP_TCPIP_TASK_STACK_SIZE=2048    # 3072→2048
CONFIG_ESP_SYSTEM_EVENT_QUEUE_SIZE=16     # 32→16
```

**风险**: 中。需测试确保不栈溢出。建议开启 `CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK` 用于调试验证。

### 3.7 缩减应用任务栈 — 预计节省 ~6-8 KB

需修改源代码中的 `xTaskCreate` 参数：

| 任务 | 当前 | 建议 | 文件:行 |
|------|------|------|---------|
| `main_loop` | 6144 | 4096 | app_main.c:345 |
| `prov_wifi` | 6144 | 4096 | app_main.c:152 |
| `ota_remote` | 6144 (OTA_TASK_STACK_SIZE) | 4096 | ota_service.c:27 |
| `ota_health` | 4096 | 3072 | ota_service.c:242 |
| `cache_flush` | 4096 | 3072 | mqtt_handler.c:61 |

```c
// app_main.c
xTaskCreate(main_loop_task, "main_loop", 4096, NULL, 5, NULL);
xTaskCreate(prov_connect_task, "prov_wifi", 4096, NULL, 5, NULL);

// ota_service.c
#define OTA_TASK_STACK_SIZE 4096U
// ota_health: 3072

// mqtt_handler.c
#define OFFLINE_FLUSH_TASK_STACK 3072
```

**风险**: 中。需运行测试确保无栈溢出。`main_loop` 中有 JSON 拼接，4096 可能够用但偏紧。

---

## 4. 汇总

| 序号 | 优化项 | 节省内存 | 风险 | 改动范围 |
|------|--------|---------|------|---------|
| 3.1 | 关闭 IPv6 | ~10-15 KB | 无 | sdkconfig.defaults |
| 3.2 | 缩减 lwIP 连接数 | ~8-12 KB | 低 | sdkconfig.defaults |
| 3.3 | 关闭 DHCP Server | ~3-5 KB | 无 | sdkconfig.defaults |
| 3.4 | 缩减 MQTT 栈+缓冲 | ~4-8 KB | 低 | sdkconfig.defaults |
| 3.5 | 缩减 NimBLE | ~5-8 KB | 低 | sdkconfig.defaults |
| 3.6 | 缩减系统任务栈 | ~5-8 KB | 中 | sdkconfig.defaults |
| 3.7 | 缩减应用任务栈 | ~6-8 KB | 中 | 源代码 |
| | **合计** | **~40-65 KB** | | |

优化后运行时可用堆预计从 ~80-100KB 提升到 **~130-150KB**。

---

## 5. 屏幕内存需求参考

| 方案 | 额外内存 | 说明 |
|------|---------|------|
| SPI 屏 (ST7789/ILI9341) + 行缓冲 | ~10-20 KB | 推荐方案，局部刷新 |
| SPI 屏 + 全帧缓冲 (240×320 RGB565) | ~150 KB | 不可行，即使优化后也紧 |
| LVGL + 无帧缓冲模式 | ~20-30 KB | 可行，需配置 `LV_DISP_RENDER_MODE_DIRECT` |
| 按键 GPIO | < 1 KB | 可忽略 |

---

## 6. 操作步骤

1. 在 `sdkconfig.defaults` 末尾追加上述 `CONFIG_*` 行
2. 执行 `idf.py fullclean`（清除旧 sdkconfig）
3. 执行 `idf.py build` 重新生成
4. 烧录后通过 `heap_caps_get_free_size(MALLOC_CAP_8BIT)` 验证可用堆
5. 压力测试：WiFi 断连重连 + MQTT 持续通信 + BLE 配网 + OTA 全流程
6. 如开启任务栈缩减（3.6/3.7），建议同时开启：
   ```ini
   CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK=y
   ```
   用于检测栈溢出
