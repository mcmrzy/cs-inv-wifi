# MQTT 优先 / 蓝牙按需切换实施计划

**目标：**修复 0.6.37 在 MQTT 已连接后恢复 BLE 引起的持续内存耗尽。用户已确认优先保证 MQTT，蓝牙改为按需切换；DSP 未接线，本次不修改 DSP。

**架构：**云端与本地 BLE 使用互斥资源模式。已有 Wi-Fi 配置时默认仅云端；菜单“手机连接”开启本地模式，无 Wi-Fi 配置时自动开放 BLE。进入本地前，等待 MQTT 握手与离线补传任务结束，停止并销毁 MQTT 客户端、释放预留栈和 TLS；返回云端前完整暂停 BLE 并回收 host/controller。所有资源切换持有已有 OTA 排他许可，不能抢占进行中的本地/远程 OTA。BLE 初始化增加内部 RAM 准入和异步就绪/错误观测，失败不能冒充已成功恢复。

## Task 1：主模式切换与回归

- [ ] `tests/mqtt_local_mode_test.py` 先复现 connected 后仍恢复 BLE，记录 RED。
- [ ] `main/app/app_main.c`：替换周期共存窗口为云端优先/明确本地模式，保留 startup 栈回收、DHCP 门控、失败退避。
- [ ] `main/app/app_network.h`：页面只发布原子模式请求，不在 LCD/蓝牙回调里执行停止/释放。
- [ ] `main/mqtt/mqtt_handler.c/.h`：本地暂停阻止新补传，等待已有补传退出，再销毁 MQTT；退出本地后重建一次，保留完整 8192 字节栈和 TLS 16KB/2KB。
- [ ] `main/direct/direct_panel_runtime.inc`：复用现有“手机连接”页面请求本地模式；离开页面后，活跃手机/配网/OTA 继续受保护，空闲超时回到云端。
- [ ] 更新原有 startup/connection host tests 的期望，不保留“connected 后开启 BLE”的旧假设。

## Task 2：BLE 准入与真实就绪（独立实现者）

- [ ] `components/ble_prov/ble_prov.c/.h`：初始化/恢复前检查内部可用 RAM 与连续块，低于 headroom 门限不尝试 NimBLE 初始化。
- [ ] 观测 host sync/广播建立成功和失败，不把创建 host task 等同实际就绪。
- [ ] `tests/ble_ota_memory_lifecycle_test.py`：覆盖资源不足、恢复失败、真实就绪和现有 owner/barrier 回归，RED→GREEN。

## Task 3：审查、构建与交付

- [ ] 独立审查模式切换、OTA TOCTOU、client/cache task 生命周期及蓝牙资源边界。
- [ ] 运行 MQTT/BLE/TLS/OTA 定向回归；不修改或重新诊断 DSP。
- [ ] 独立构建 `0.6.38-mqtt-first`，核验嵌入版本、镜像校验、分区不变、交付 SHA-256。
- [ ] 仅交付应用固件与说明；不刷机、不部署服务、不提交/推送大量现有未提交改动。

## 验收与限制

主机测试验证真实 C 模式决策与生命周期，不模拟真实射频/完整 NimBLE 内存峰值。实机需默认云端运行至少 5 分钟，无自动 BLE 启动及分配失败；随后验证进入/离开“手机连接”、配网和本地 OTA 后自动回云端。当前实机日志证明 0.6.37 能认证 MQTT，但不能证明在线稳定或遥测入库。

## 本地交付结果

已完成上述实现、先 RED 后 GREEN、独立审查及其两个 P2 问题修复（在途缓存有界退出、OTA 中仍允许云模式重连）。最终 10 组定向回归通过，包括本地模式 5 项和 BLE 生命周期 14 项。ESP-IDF 5.5.4 编译通过，应用 1,618,464 字节、OTA 余量 85,472 字节；分区表与上一版一致，镜像校验有效，嵌入版本 `0.6.38-mqtt-first`。

应用 SHA-256：`ea1efbfd1adca0c3a43c8ee4f7dabb2965e4bd5ef0ef53313ffc697c218b62e8`。交付目录 `release/esp32c3_v0.6.38_mqtt_first_20261019`。仅本地交付，尚未刷机、未确认实机稳定在线、未部署服务器、未提交/推送。
