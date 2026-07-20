# ESP32-C2 固件稳定化实施计划

> 对应设计：`docs/superpowers/specs/2026-07-19-esp32c2-firmware-stabilization-design.md`

**目标：** 修复 ESP32-C2 已确认的事件、MQTT、OTA、NVS、主循环和离线缓存问题；删除 AP 网页；保留远程 MQTT OTA 与本地 AP/App OTA，并让两者共用一个安全 OTA 内核。

**约束：** 目标为 ESP-IDF 5.5.x、ESP32-C2、4 MiB Flash。现有工作区包含用户未提交改动，只修改 `esp32c2_idf/`、`tools/tests/` 和本计划列出的文档，不覆盖 Arduino 与模拟器改动。

**验证基线：** 当前 C2 Ninja 构建成功，app binary 约 `0x12e0f0`，单 OTA slot 为 `0x1c0000`；现有 Node 契约测试 15 项通过。

---

## 任务 1：增加会失败的架构与安全契约测试

**文件：**

- 修改：`tools/tests/firmware_contract.test.js`
- 修改：`tools/tests/ota_signature.test.mjs`
- 新增：`tools/tests/esp32c2_ota_contract.test.mjs`

**步骤：**

1. 增加断言：C2 签名实现不能包含恒真 stub，必须调用真实 Ed25519/PSA 验签 API。
2. 增加断言：Wi-Fi/IP 事件处理先判断 event base。
3. 增加断言：`tcp_provision_process()` 不再出现在主循环和状态机 tick。
4. 增加断言：页面 HTML 标记和 `/status`、`/ota` 页面 handler 被移除，App API 路由仍存在。
5. 增加断言：远程 MQTT 和本地 HTTP 都提交到 `ota_service`。
6. 增加签名负例：清单任意字段被修改、Base64 长度非法和 SHA 大写时拒绝。
7. 运行测试并确认新增项因现有实现失败，而不是语法或 fixture 错误。

命令：

```powershell
node --test tools\tests\firmware_contract.test.js tools\tests\ota_signature.test.mjs tools\tests\esp32c2_ota_contract.test.mjs
```

## 任务 2：建立纯数据 OTA 清单与统一状态模型

**文件：**

- 新增：`esp32c2_idf/main/ota_service.h`
- 新增：`esp32c2_idf/main/ota_service.c`
- 修改：`esp32c2_idf/main/CMakeLists.txt`
- 测试：`tools/tests/esp32c2_ota_contract.test.mjs`

**步骤：**

1. 在头文件定义 `OtaSource`、`OtaTarget`、`OtaState`、`OtaManifest`、`OtaSnapshot` 和稳定错误码。
2. 实现有界字符串复制、target/size/SHA/security version/timeout 校验。
3. 实现单任务互斥、状态转换、进度快照复制和取消标志。
4. 所有锁只保护内存状态；回调、HTTP、MQTT 和 Flash I/O 均在锁外执行。
5. 添加契约测试验证状态、字段和单任务约束存在。
6. 构建，先只链接空 worker 和状态 API，保持行为不变。

## 任务 3：实现真实单次 Ed25519 验签

**文件：**

- 修改：`esp32c2_idf/main/ota_signature.c`
- 修改：`esp32c2_idf/main/ota_signature.h`
- 修改：`esp32c2_idf/main/CMakeLists.txt`
- 修改：`esp32c2_idf/main/Kconfig.projbuild`
- 修改：`esp32c2_idf/sdkconfig.defaults`
- 测试：`tools/tests/ota_signature.test.mjs`

**步骤：**

1. 保留 `CS_INV_OTA_V1` 规范化消息，拒绝超长或非规范字段。
2. 严格 Base64 解码，要求 64 字节签名。
3. 使用 ESP-IDF Mbed TLS/PSA Crypto PureEdDSA 导入 32 字节 Ed25519 公钥并执行一次验签。
4. 检查每一个 PSA 返回值，退出前销毁 key handle 并清理临时缓冲。
5. 若目标 IDF 配置不支持 PureEdDSA，停止并选择小型已验证组件，不能恢复到放行 stub。
6. 运行固定向量和篡改负例，再执行 C2 构建。

## 任务 4：分目标安全版本与 ESP pending 元数据

**文件：**

- 修改：`esp32c2_idf/main/nvs_storage.h`
- 修改：`esp32c2_idf/main/nvs_storage.c`
- 修改：`esp32c2_idf/main/ota_service.c`
- 测试：`tools/tests/esp32c2_ota_contract.test.mjs`

**步骤：**

1. 新增 ESP、ARM 已确认安全版本读写 API。
2. 新增 ESP pending 结构读写/清除 API，保存安全版本、task ID、source、version 和目标分区 subtype/address 标识。
3. 兼容迁移旧 `ota_sec_ver`：仅在新 key 都不存在时读取一次，并以保守策略迁移，不覆盖已存在的新 key。
4. 修复 Wi-Fi、MQTT、SN、key、AP SSID setter，任何 `nvs_set_*` 或 `nvs_commit` 失败都返回 false。
5. 调用端只有在持久化成功后发送 ACK 或成功 JSON。
6. 构建并检查 NVS key 名不超过 15 字符。

## 任务 5：迁移远程 MQTT OTA 到统一内核

**文件：**

- 重构：`esp32c2_idf/main/mqtt_ota_handler.c`
- 修改：`esp32c2_idf/main/mqtt_ota_handler.h`
- 扩展：`esp32c2_idf/main/ota_service.c`
- 修改：`esp32c2_idf/main/arm_ota.c`
- 修改：`esp32c2_idf/main/arm_ota.h`

**步骤：**

1. MQTT handler 只保留 JSON 解析、命令上下文和状态/错误发布。
2. `start` 构造 manifest 并调用 `ota_service_start_remote()`；`query` 读取快照；`cancel` 调用统一取消。
3. 把 HTTPS download worker、ESP OTA 写入和 ARM cache 暂存迁入 `ota_service`。
4. 开始下载前执行一次清单验签；接收时用 4 KiB buffer 流式 SHA-256。
5. ESP 摘要通过后才 `esp_ota_end`、写 pending 和设置 boot partition。
6. ARM 摘要通过后才从 `/cache/arm_ota.bin` 发送 `CMD_ARM_OTA_START/DATA/END`。
7. 使用 manifest timeout 60-3600 秒；删除写死 300/600 秒逻辑。
8. 每个 query/cancel 使用局部 request context，不再覆盖 worker task ID。
9. 检查 worker `xTaskCreate` 返回值；创建失败立即恢复 idle 并上报错误。
10. 运行契约测试和 C2 构建。

## 任务 6：用 `esp_http_server` 替换 AP 网页/socket 服务器

**文件：**

- 新增：`esp32c2_idf/main/ap_api_server.h`
- 新增：`esp32c2_idf/main/ap_api_server.c`
- 修改：`esp32c2_idf/main/CMakeLists.txt`
- 修改：`esp32c2_idf/main/app_main.c`
- 修改：`esp32c2_idf/main/state_machine.c`
- 删除：`esp32c2_idf/main/tcp_provision.c`
- 删除：`esp32c2_idf/main/tcp_provision.h`
- 测试：`tools/tests/esp32c2_ota_contract.test.mjs`

**步骤：**

1. 使用 `esp_http_server` 注册 `/scan`、`/config`、`/wifi_status`、`/ota/info`、`/ota/progress`、`/ota/upload` 和 `/ota/cancel`。
2. 迁移 AP 启停和 Wi-Fi scan 数据，不迁移 HTML/CSS/JavaScript。
3. `/config` 支持 JSON 与现有 URL encoded body，共用输入校验和 NVS 保存。
4. `/ota/upload` 读取 `X-OTA-*` headers 和 raw body，调用 `ota_service_start_local()`；header/size/签名不合格时不读取固件到 Flash。
5. ESP body 写非活动分区；ARM body 写 `/cache/arm_ota.bin`。两者流式 SHA-256 并更新统一进度。
6. HTTP 连接中断调用 abort/cancel，释放全局 OTA 占用。
7. `/ota/progress` 序列化真实快照；所有未注册页面路由返回 JSON 404。
8. 从主循环和状态机删除所有 `tcp_provision_process()` 调用。
9. 运行路由契约测试和 C2 构建。

## 任务 7：启用 ESP rollback 与启动健康确认

**文件：**

- 修改：`esp32c2_idf/sdkconfig.defaults`
- 修改：`esp32c2_idf/sdkconfig`
- 修改：`esp32c2_idf/main/app_main.c`
- 修改：`esp32c2_idf/main/ota_service.c`

**步骤：**

1. 启用 bootloader app rollback。
2. 启动时读取当前 app 状态和 pending 元数据。
3. 对 `PENDING_VERIFY` 镜像启动 60 秒健康确认计时，不依赖外部网络可达。
4. NVS、UART、状态机和主循环存活条件满足后 mark valid，再提升 ESP confirmed security version。
5. 旧镜像启动且 pending 分区不匹配时记录 rolled_back、清 pending，保持 confirmed version 不变。
6. pending MQTT 任务在连接恢复后补报最终状态。
7. 构建并检查 bootloader/app 配置一致。

## 任务 8：修复 Wi-Fi 事件与 MQTT 生命周期

**文件：**

- 修改：`esp32c2_idf/main/app_main.c`
- 修改：`esp32c2_idf/main/state_machine.c`
- 修改：`esp32c2_idf/main/state_machine.h`
- 修改：`esp32c2_idf/main/mqtt_handler.c`
- 修改：`esp32c2_idf/main/mqtt_handler.h`
- 测试：`tools/tests/firmware_contract.test.js`

**步骤：**

1. 事件回调先按 `WIFI_EVENT`、`IP_EVENT` 分支，再转换 event data。
2. 临时 Wi-Fi 断线只通知 MQTT 网络不可用，不 destroy client。
3. 增加显式 `mqtt_reconfigure()`；仅 SN、key、broker/凭据变化时 stop/destroy/init/start。
4. 无 key 启动返回配置未就绪；成功写 key 后状态机立即触发 init。
5. MQTT connected/disconnected 通过状态机事件处理，减少跨任务可变全局。
6. 检查 event register、task create、queue create 返回值；必要组件失败时进入可诊断错误状态。
7. 运行契约测试和 C2 构建。

## 任务 9：修复离线缓存批量与 QoS 1 PUBACK 语义

**文件：**

- 修改：`esp32c2_idf/main/offline_cache.c`
- 修改：`esp32c2_idf/main/offline_cache.h`
- 修改：`esp32c2_idf/main/mqtt_handler.c`
- 测试：`tools/tests/esp32c2_ota_contract.test.mjs`

**步骤：**

1. 恢复最多 10 条或 60 秒批量刷盘，离线/低内存路径可强制刷盘。
2. 使用临时文件与 rename 提交批次。
3. flush 在锁内只选择小批记录，锁外调用 publish。
4. 保存 publish 返回的 MQTT msg_id 与缓存记录映射。
5. `MQTT_EVENT_PUBLISHED` 到来后才确认并删除 QoS 1 记录。
6. 断线时清除 in-flight 映射但保留磁盘记录，重连后重发。
7. 明确处理损坏文件、容量淘汰计数和 `last_sync_time` 互斥。
8. ARM OTA 暂存前调用 reserve API；暂存文件存在时保护预留空间。
9. 运行测试和 C2 构建。

## 任务 10：清理重复实现和接口

**文件：**

- 删除：`esp32c2_idf/main/ota_manager.c`
- 删除：`esp32c2_idf/main/ota_manager.h`
- 修改：`esp32c2_idf/main/CMakeLists.txt`
- 修改：`esp32c2_idf/main/app_main.c`
- 修改：`esp32c2_idf/main/state_machine.c`
- 修改：`esp32c2_idf/main/arm_ota.h`
- 修改：相关 OTA 文档

**步骤：**

1. 将仍被使用的版本查询 helper 迁入 `ota_service` 或目标模块。
2. 删除所有 `ota_manager_*` 引用和源码。
3. 删除 `arm_ota_end_streaming_cancelable` 重复声明。
4. 用 `rg` 确认不存在旧 page HTML、旧 socket process 和旧安全版本 API 调用。
5. 更新 MQTT OTA 文档中的 HTTPS、签名、安全版本和统一状态说明。

## 任务 11：完整验证与精简审查

**文件：** 本轮所有改动。

**步骤：**

1. 运行全部 Node 测试。
2. 执行 ESP32-C2 clean build；若 `idf.py` 环境仍不匹配，记录环境问题并用同一 IDF 工具链直接 Ninja 验证。
3. 检查 map/size：binary 必须小于 `0x1c0000`，记录剩余 slot 和静态 RAM。
4. 检查任务 stack high-water mark 埋点或配置，确认 HTTP/OTA 任务没有明显过配。
5. 运行 `rg` 静态检查：无验签 stub、无页面内容、无主循环 socket 阻塞、无旧安全版本共用 API。
6. 按 simplify 流程检查重复分支、无效包装和可缩小的临界区，并只修复有明确收益的问题。
7. 按 verify 与 verification-before-completion 流程重新执行完整测试和构建。
8. 若没有真机，明确列出待执行的板级验收，不声称真机问题已闭环。

---

## 完成标准

- 远程 MQTT OTA 与本地 AP/App OTA 均存在并共用 `ota_service`。
- 一次 OTA 只验一份 Ed25519 发布签名；数据只做流式 SHA-256。
- AP 不再提供网页，但配网与 OTA API 可用。
- Wi-Fi 短断线后 MQTT 生命周期可恢复；运行中写 key 可上线。
- ESP rollback、生效后安全版本确认、ARM 独立安全版本均落地。
- 主循环没有阻塞网络 I/O。
- 离线 QoS 1 数据在 PUBACK 前不删除。
- 自动化测试与 ESP32-C2 构建通过，资源保持在 4 MiB 分区限制内。
