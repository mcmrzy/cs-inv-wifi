# BLE持续广播实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use compose:subagent (recommended) or compose:execute to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让BLE广播像WiFi AP一样持续广播，而不需要按下按钮触发

**Architecture:** 在WiFi启动后立即初始化BLE配网，使其持续广播。需要优化内存配置以支持WiFi和BLE共存。

**Tech Stack:** ESP-IDF v5.5, NimBLE, WiFi Provisioning

## Global Constraints

- ESP32-C2内存有限（约31KB可用）
- WiFi和BLE需要共存
- BLE广播服务名格式：CS_INV + SN后6位

---

### Task 1: 优化内存配置

**Covers:** 内存优化以支持WiFi+BLE共存

**Files:**
- Modify: `D:\CS_INV_WIFI\esp32c2_idf\sdkconfig`

**Interfaces:**
- Consumes: 当前WiFi缓冲区配置
- Produces: 优化后的内存配置

- [ ] **Step 1: 减少WiFi缓冲区数量**

修改sdkconfig，减少WiFi缓冲区以释放内存：
```
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=3
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=6
CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=4
CONFIG_ESP_WIFI_RX_BA_WIN=3
```

- [ ] **Step 2: 减少BLE缓冲区数量**

修改sdkconfig，减少BLE缓冲区：
```
CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT=8
CONFIG_BT_NIMBLE_MSYS_2_BLOCK_COUNT=4
CONFIG_BT_NIMBLE_TRANSPORT_ACL_FROM_LL_COUNT=6
CONFIG_BT_NIMBLE_TRANSPORT_EVT_COUNT=6
```

- [ ] **Step 3: 验证内存改善**

编译并烧录固件，检查内存使用情况：
```
W (xxx) app: Heap after App API    free=XXXXX largest=XXXXX min=XXXXX
```

---

### Task 2: 修改app_main.c添加BLE初始化

**Covers:** 在启动时初始化BLE并开始广播

**Files:**
- Modify: `D:\CS_INV_WIFI\esp32c2_idf\main\app_main.c`

**Interfaces:**
- Consumes: `ble_prov_start()` from `ble_prov.h`
- Produces: BLE在启动时自动开始广播

- [ ] **Step 1: 添加BLE头文件引用**

在app_main.c顶部添加：
```c
#include "ble_prov.h"
```

- [ ] **Step 2: 在WiFi启动后添加BLE初始化**

在`esp_wifi_start()`成功后，添加BLE初始化代码：
```c
// 启动BLE持续广播
if (storage_has_sn()) {
    ble_prov_set_callback(on_ble_provision_done);
    if (ble_prov_start(g_device_sn, FIRMWARE_VERSION)) {
        ESP_LOGI(TAG, "BLE advertising started: %s", ble_prov_get_service_name());
    } else {
        ESP_LOGW(TAG, "BLE advertising start failed");
    }
}
```

- [ ] **Step 3: 添加BLE配网回调函数**

在app_main.c中添加BLE配网完成回调：
```c
static void on_ble_provision_done(const char *ssid, const char *password)
{
    ESP_LOGI(TAG, "BLE provision received: SSID=%s", ssid);
    
    // 保存WiFi配置
    storage_set_wifi(ssid, password);
    
    // 重启以应用新配置
    esp_restart();
}
```

---

### Task 3: 修改state_machine.c移除按钮触发逻辑

**Closes:** 移除按钮触发BLE的逻辑，改为持续广播

**Files:**
- Modify: `D:\CS_INV_WIFI\esp32c2_idf\main\state_machine.c`

**Interfaces:**
- Consumes: `ble_prov_start()`, `ble_prov_stop()`
- Produces: 简化的按钮处理逻辑

- [ ] **Step 1: 修改按钮长按处理**

将按钮长按处理改为工厂重置，移除BLE触发逻辑：
```c
} else if (duration > 200 && duration < BUTTON_LONG_PRESS) {
    // 短按：无操作或LED指示
    ESP_LOGI(TAG, "Short press detected");
}
```

- [ ] **Step 2: 移除BLE配网相关代码**

移除以下代码：
- `ble_prov_get_state()` 检查
- `ble_prov_start()` 调用
- `ble_prov_set_callback()` 调用

- [ ] **Step 3: 验证功能**

1. 编译并烧录固件
2. 检查BLE广播是否在启动时自动开始
3. 使用手机APP测试BLE配网功能

---

## 验证步骤

1. **编译验证**
   ```
   idf.py build
   ```

2. **烧录并监控**
   ```
   idf.py flash monitor
   ```

3. **检查日志输出**
   ```
   I (xxx) app: BLE advertising started: CS_INV_XXXXXX
   ```

4. **手机APP测试**
   - 打开"ESP BLE Provisioning" APP
   - 搜索设备名称：CS_INV_XXXXXX
   - 测试配网功能

## 注意事项

1. **内存限制**：ESP32-C2内存有限，如果BLE启动失败，检查内存是否足够
2. **WiFi共存**：BLE和WiFi共存时，性能可能受影响
3. **重启机制**：BLE配网成功后会重启设备以应用新配置
