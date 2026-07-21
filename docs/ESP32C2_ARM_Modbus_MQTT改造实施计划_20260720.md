# ESP32-C2 从 ARM 读取 Modbus 并上报 MQTT——实施与改造计划

> 日期：2026-07-20  
> 状态：讨论稿 / 可执行计划  
> 推荐架构：ESP32-C2 = Modbus RTU Master；ARM = Modbus RTU Slave；MQTT 外部契约保持不变  
> 迁移策略：先双协议兼容验证，再切换生产数据链路；ARM OTA 暂时保留现有专用帧协议

## 0. 文件树变更清单（先看这个）

```text
CS_INV_WIFI/
  [M] esp32c2_idf/main/CMakeLists.txt
  [M] esp32c2_idf/main/Kconfig.projbuild
  [M] esp32c2_idf/main/config.h
  [M] esp32c2_idf/main/app_main.c
  [M] esp32c2_idf/main/state_machine.c
  [M] esp32c2_idf/main/state_machine.h
  [M] esp32c2_idf/main/uart_protocol.c
  [M] esp32c2_idf/main/uart_protocol.h
  [M] esp32c2_idf/main/arm_ota.c
  [M] esp32c2_idf/main/mqtt_handler.c
  [A] esp32c2_idf/main/uart_bus_owner.c
  [A] esp32c2_idf/main/uart_bus_owner.h
  [A] esp32c2_idf/main/modbus_rtu_master.c
  [A] esp32c2_idf/main/modbus_rtu_master.h
  [A] esp32c2_idf/main/modbus_register_map.c
  [A] esp32c2_idf/main/modbus_register_map.h
  [A] esp32c2_idf/main/arm_data_model.c
  [A] esp32c2_idf/main/arm_data_model.h
  [A] esp32c2_idf/main/modbus_poll_scheduler.c
  [A] esp32c2_idf/main/modbus_poll_scheduler.h
  [A] esp32c2_idf/main/modbus_mqtt_mapper.c
  [A] esp32c2_idf/main/modbus_mqtt_mapper.h
  [A] esp32c2_idf/test/test_modbus_rtu_master.c
  [A] esp32c2_idf/test/test_modbus_register_map.c
  [A] esp32c2_idf/test/test_modbus_mqtt_mapper.c
  [M] tools/tests/firmware_contract.test.js
  [A] tools/tests/modbus_contract.test.mjs
  [M] tools/arm_simulator/lib/services/uart_service.dart
  [A] tools/arm_simulator/lib/protocol/modbus_rtu_codec.dart
  [A] tools/arm_simulator/lib/protocol/modbus_register_bank.dart
  [A] tools/arm_simulator/test/modbus_rtu_codec_test.dart
  [A] tools/arm_simulator/test/modbus_register_bank_test.dart
  [A] docs/ARM_ESP32_Modbus_Register_Map.md
  [M] docs/ARM_ESP32_UART_Protocol.md
  [M] docs/MQTT协议文档.md

ARM_FIRMWARE/（外部 ARM 工程，实际根目录与文件名需在 Gate 0 确认）
  [A] communication/modbus_rtu_slave.c
  [A] communication/modbus_rtu_slave.h
  [A] communication/modbus_registers.c
  [A] communication/modbus_registers.h
  [A] communication/modbus_snapshot.c
  [A] communication/modbus_snapshot.h
  [M] application/telemetry_task.c
  [M] application/control_task.c
  [M] application/uart_task.c
  [M] bootloader_or_iap/arm_ota_transport.c
  [A] tests/test_modbus_registers.c
  [A] tests/test_modbus_control_mailbox.c
```

说明：

1. `[M]` 表示修改现有文件。
2. `[A]` 表示新增文件。
3. 本计划不直接删除旧 UART 协议代码；旧协议先作为兼容和 ARM OTA 通道保留。
4. ARM 工程当前不在本工作区，ARM 路径是职责明确的逻辑占位。开始实施前必须替换为真实路径。

---

## 1. 设计目标

1. 将 ARM→ESP 的业务数据链路从“ARM 主动发送自定义 JSON 帧”改为“ESP 主动轮询 ARM Modbus 寄存器”。
2. ESP32-C2 使用 UART1（GPIO0 TX、GPIO1 RX）作为 Modbus RTU Master，ARM 使用同一物理串口作为 Slave。
3. MQTT Topic、JSON 字段、QoS、`task_id`、命令回复和 OTA 状态等云端契约保持不变，后端和 App 不因底层 UART 协议迁移而改变。
4. ARM 使用稳定、版本化的寄存器表发布逆变器、电池、光伏、能量、状态、告警和设备信息。
5. MQTT 下行命令通过 Modbus 控制邮箱写入 ARM；ARM 异步执行后返回与请求序号关联的结果。
6. 保留现有 ESP OTA 与 ARM OTA 能力。ARM OTA 期间暂停 Modbus 轮询，独占 UART，继续使用现有 `0x10～0x15` 专用 OTA 帧。
7. 通过快照序号、CRC16、超时重试、连续失败计数和数据新鲜度控制，避免半更新数据、误控制和离线误报。
8. Modbus 实现保持小内存：固定 ADU 缓冲不超过 256 B，不引入动态大缓冲，不降低 BLE/AP/MQTT 可用内存水位。
9. 支持分阶段联调：PC/Flutter ARM 模拟器、ESP 单元测试、串口硬件在环和云端 MQTT 行为逐层验证。

### 1.1 明确不在本次首期范围内

1. 不把 MQTT JSON 原文塞入 Modbus 寄存器。
2. 不在首期使用 Modbus TCP。
3. 不在首期将 ARM OTA 改成 Modbus 文件记录或自定义功能码。
4. 不改变云端 MQTT Topic 和数据库字段语义。
5. 不允许 ESP 与 ARM 同时主动发送；业务态由 ESP Master 发起全部 Modbus 事务。
6. 不在寄存器中保存 MQTT TLS 私钥、设备密钥或 Wi-Fi 密码。

---

## 2. 关键约束与协议约定

### 2.1 Gate 0：实施前必须确认

1. ARM 固件是否允许修改，并能实现 Modbus RTU Slave。
2. ARM 是否已经存在寄存器表；若存在，以现有表为输入进行映射，禁止另建冲突地址。
3. ARM MCU 型号、UART 外设、DMA/中断方式、RTOS/裸机模型和可用 RAM/Flash。
4. ARM 工程真实目录、构建命令、烧录方式和现有自动化测试方式。
5. ESP↔ARM 是 3.3 V TTL 点对点，还是经过 RS485 收发器；若为 RS485，必须确认 DE/RE 控制 GPIO 和收发切换时序。
6. ARM OTA 是应用内 IAP 还是独立 Bootloader；进入 IAP 后是否仍保持相同 UART 参数。
7. 最终字段清单和缩放倍率必须由 ARM 数据结构与 `docs/系统参数规范_48V离网逆变器.md` 双向核对。

Gate 0 未完成时，只允许实现纯协议 codec 和模拟器，不进入真实寄存器映射和 ARM 控制写入。

### 2.2 Modbus RTU 基线参数

| 参数 | 约定 |
|---|---|
| 角色 | ESP32-C2 Master；ARM Slave |
| Slave 地址 | 默认 `0x01`，预留 NVS/Kconfig 配置 |
| 物理层 | 默认 3.3 V TTL UART 点对点；RS485 作为硬件变体 |
| 串口 | UART1，TX=GPIO0，RX=GPIO1 |
| 波特率 | 115200 |
| 数据格式 | 8N1，无硬件流控 |
| 校验 | Modbus CRC16，低字节先发送 |
| 响应超时 | 初始 100 ms，硬件联调后按 ARM 最坏响应时间校准 |
| 单请求重试 | 2 次 |
| 最大连续失败 | 3 个轮询周期后判定 ARM 通信离线 |
| 最大读寄存器数 | 每请求不超过 125 个 16-bit 寄存器 |
| 最大写寄存器数 | 每请求不超过 123 个 16-bit 寄存器 |
| 广播地址 | 首期禁止使用 `0x00` 广播写 |

115200 8N1 下 1 字节约占 86.8 μs。一次读取 100 个寄存器的完整请求与响应通常约 20 ms，足以支持关键状态 200 ms、主要遥测 1 s 的轮询周期。

### 2.3 功能码约定

| 功能码 | 用途 |
|---|---|
| `0x04` Read Input Registers | 读取遥测、设备信息、状态、能量、告警和只读结果 |
| `0x03` Read Holding Registers | 读取配置、控制邮箱和命令执行状态 |
| `0x06` Write Single Register | 仅用于明确无参数、幂等的简单控制；首期可不开放 |
| `0x10` Write Multiple Registers | 写控制参数、请求序号和提交标志，作为主要下行控制方式 |
| Exception Response | ARM 按标准返回非法功能、非法地址、非法值和设备忙 |

### 2.4 数据类型与字节序

1. 单寄存器按 Modbus 标准使用大端字节序。
2. 32 位值使用两个连续寄存器，高 16 位在低地址，低 16 位在高地址。
3. 64 位累计量使用四个连续寄存器，从最高字到最低字排列。
4. 有符号量使用二进制补码 `int16_t`/`int32_t`。
5. 首期不在寄存器中传 IEEE-754 float；使用整数 + 固定倍率，避免 ARM/ESP 浮点和字序歧义。
6. 常用倍率：电压/电流/功率因数/温度按字段定义使用 ×10、×100 或 ×1000；每个字段在寄存器表中必须显式标注。
7. ASCII 字符串每寄存器放两个字节，高字节在前；固定长度区域不足部分填 `0x00`。
8. 未实现寄存器必须返回 Modbus Exception `0x02`，禁止默默返回随机值。

### 2.5 初版寄存器地址规划

以下是规划区间，不是最终字段表。最终地址由 `docs/ARM_ESP32_Modbus_Register_Map.md` 固化。

| 地址范围 | 类型 | 内容 | 建议轮询 |
|---|---|---|---|
| `0x0000～0x001F` | Input | 协议魔数、寄存器表版本、ARM 固件版本、能力位、运行时间 | 启动及 60 s |
| `0x0020～0x003F` | Input | SN、产品型号、硬件版本、设备标识 | 启动及变更时 |
| `0x00F0～0x00FF` | Input | 全局快照序号、数据更新时间、健康状态 | 200 ms～1 s |
| `0x0100～0x013F` | Input | AC 输入/输出、电网、负载数据 | 1 s |
| `0x0140～0x017F` | Input | 电池/BMS 汇总数据 | 1 s |
| `0x0180～0x01BF` | Input | PV/MPPT 数据 | 1 s |
| `0x01C0～0x01FF` | Input | 逆变器状态、模式、开关状态 | 200 ms |
| `0x0200～0x023F` | Input | 日/月/总能量、运行时长 | 10～60 s |
| `0x0240～0x02FF` | Input | 电芯电压、温度等数组 | 10～30 s |
| `0x0300～0x033F` | Input | 当前告警位图、故障码、事件序号 | 200 ms |
| `0x1000～0x103F` | Holding | 控制请求邮箱 | MQTT 命令触发 |
| `0x1040～0x107F` | Holding/Input | 控制结果邮箱与 ARM 忙状态 | 命令等待期间 50～100 ms |
| `0x1100～0x117F` | Holding | 允许远程设置的设备参数 | 按需 |
| `0x1200～0x123F` | Holding | 协议诊断计数、最后异常 | 5～60 s |

关键管理寄存器建议：

| 地址 | 名称 | 说明 |
|---|---|---|
| `0x0000` | `PROTOCOL_MAGIC` | 固定 `0x4353`（ASCII `CS`） |
| `0x0001` | `REGISTER_MAP_VERSION` | 高字节 major，低字节 minor |
| `0x0002` | `CAPABILITY_FLAGS` | 告警、控制、能量、电芯、OTA 等能力位 |
| `0x00F0～0x00F1` | `SNAPSHOT_SEQ` | 32 位快照序号，完整刷新后递增 |
| `0x00F2～0x00F3` | `SNAPSHOT_TIME_MS` | ARM 单调毫秒低 32 位 |
| `0x0300～0x0301` | `ALARM_EVENT_SEQ` | 告警状态变化时递增 |
| `0x0302～0x0305` | `ALARM_BITMAP` | 64 位当前活动告警 |
| `0x1000～0x1001` | `CONTROL_REQ_SEQ` | ESP 生成的 32 位请求序号 |
| `0x1002` | `CONTROL_CMD_ID` | 枚举化命令 ID |
| `0x1003` | `CONTROL_ARG_COUNT` | 有效参数寄存器数量 |
| `0x1004～0x1013` | `CONTROL_ARGS` | 最多 16 个参数寄存器 |
| `0x101F` | `CONTROL_COMMIT` | ESP 最后写入固定提交值，ARM 才执行 |
| `0x1040～0x1041` | `CONTROL_RESP_SEQ` | 完成后回写对应请求序号 |
| `0x1042` | `CONTROL_STATE` | idle/pending/running/success/failed |
| `0x1043` | `CONTROL_ERROR` | 稳定错误码，不传自由文本 |
| `0x1044～0x1053` | `CONTROL_RESULT` | 命令结果参数 |

### 2.6 快照一致性

ARM 不能让 ESP 读到一半旧值、一半新值。推荐 ARM 使用双缓冲或临界区构建寄存器快照：

1. ARM 业务任务将实时数据写入内部数据模型。
2. 快照任务复制全部对外字段到非活动寄存器缓冲。
3. 复制完成后原子切换活动缓冲并递增 `SNAPSHOT_SEQ`。
4. ESP 读取块前读取一次 `SNAPSHOT_SEQ`，读取完成后再读一次。
5. 两次序号不同则丢弃该批数据并重读一次；连续变化时推迟到下个周期。

告警使用独立 `ALARM_EVENT_SEQ`。ESP 发现序号变化时立即读取告警块并发布 QoS 1，避免仅按当前位图比较导致快速出现/消失的告警漏报。

### 2.7 MQTT 契约保持策略

1. ESP 在 `modbus_mqtt_mapper` 中将寄存器数据转成现有 Topic Payload。
2. Topic 继续使用 `cs_inv/{sn}/data/ac`、`data/battery`、`data/pv`、`data/status`、`data/energy`、`data/cells`、`data/alarm`。
3. 字段名、单位、倍率还原后的数值类型和 envelope 版本保持不变。
4. MQTT 离线时继续进入现有 `offline_cache`；Modbus 轮询不因云端断线停止。
5. MQTT 下行 `task_id` 只存在 ESP/MQTT 层；ESP 将其映射为内部 `CONTROL_REQ_SEQ`，ARM 不存储长字符串。
6. ARM 返回成功/失败后，ESP 使用原 `task_id` 发布 `cmd/response`。
7. ESP 重启导致 task 映射丢失时，未完成命令不能伪造成功；应回复重启/状态未知，或由持久化命令表恢复关联。

### 2.8 UART 所有权与 ARM OTA

UART1 任意时刻只能有一个消费者读取 RX：

| 模式 | UART 所有者 | 协议 |
|---|---|---|
| 正常业务 | Modbus Poll Scheduler | Modbus RTU |
| 兼容迁移 | Legacy UART Adapter | `0xAA + CMD + LEN + DATA + XOR` |
| ARM OTA | ARM OTA Service | 现有 `0x10～0x15` OTA 帧 |

`uart_bus_owner` 提供独占切换：

1. 请求方申请总线所有权。
2. 停止当前调度器并等待正在执行的事务结束。
3. 清空 UART RX/TX 缓冲，重置协议解析器。
4. 切换所有者并开始目标协议。
5. OTA 完成或失败后重新初始化 Modbus 状态、探测 `PROTOCOL_MAGIC` 并恢复轮询。

禁止 Modbus parser、旧 UART parser 和 ARM OTA parser 同时调用 `uart_read_bytes()`。

---

## 3. 流程化改造说明（按执行顺序）

## 流程 1：Gate 0 调研与寄存器字段冻结

### 涉及文件

- `[A] docs/ARM_ESP32_Modbus_Register_Map.md`
- `[M] docs/ARM_ESP32_UART_Protocol.md`
- `[M] docs/MQTT协议文档.md`
- `[M] ARM_FIRMWARE/application/telemetry_task.c`
- `[M] ARM_FIRMWARE/application/control_task.c`

### 修改说明

1. 获取 ARM 工程、真实数据结构和控制接口。
2. 对照 MQTT 各 Topic 字段，建立“ARM 字段 → 寄存器 → ESP 数据模型 → MQTT 字段”四列表。
3. 为每个字段定义地址、长度、signed、倍率、单位、有效范围、无效值和更新频率。
4. 冻结寄存器表 `major.minor` 版本；地址一旦发布不得复用为不同含义。
5. 区分只读遥测、可写配置、瞬时控制和诊断计数。
6. 将旧 UART 文档标记为 Legacy/OTA 专用，不立即删除。

### 完成判定

- 每个现有 MQTT 字段都能追溯到 ARM 来源或明确标为 ESP 本地产生。
- 不存在地址重叠、未定义倍率、未知字序或缺失合法范围。
- ARM 和 ESP 开发者共同确认寄存器表版本 `1.0`。

## 流程 2：实现并验证通用 Modbus RTU Codec

### 涉及文件

- `[A] esp32c2_idf/main/modbus_rtu_master.c`
- `[A] esp32c2_idf/main/modbus_rtu_master.h`
- `[A] esp32c2_idf/test/test_modbus_rtu_master.c`
- `[A] tools/arm_simulator/lib/protocol/modbus_rtu_codec.dart`
- `[A] tools/arm_simulator/test/modbus_rtu_codec_test.dart`

### 修改说明

1. 实现 CRC16 Modbus，覆盖空输入、标准向量和单 bit 损坏。
2. 实现 `0x03`、`0x04`、`0x06`、`0x10` 请求编码和响应解析。
3. 严格校验 Slave 地址、功能码、字节数、CRC、寄存器数量和 Exception Response。
4. 使用固定大小 ADU 缓冲，禁止按响应长度无界 `malloc`。
5. 为 UART 超时、短帧、粘连垃圾、错误地址、错误 CRC 和异常响应返回稳定错误码。
6. Dart 模拟器与 C 实现共享相同测试向量，避免两端字节序漂移。

### 目标接口

- `uint16_t modbus_crc16(const uint8_t *data, size_t length)`
- `ModbusResult modbus_read_input_registers(...)`
- `ModbusResult modbus_read_holding_registers(...)`
- `ModbusResult modbus_write_multiple_registers(...)`
- `ModbusResult modbus_parse_response(...)`

## 流程 3：ARM 实现 Modbus Slave 与一致性快照

### 涉及文件

- `[A] ARM_FIRMWARE/communication/modbus_rtu_slave.c`
- `[A] ARM_FIRMWARE/communication/modbus_rtu_slave.h`
- `[A] ARM_FIRMWARE/communication/modbus_registers.c`
- `[A] ARM_FIRMWARE/communication/modbus_registers.h`
- `[A] ARM_FIRMWARE/communication/modbus_snapshot.c`
- `[A] ARM_FIRMWARE/communication/modbus_snapshot.h`
- `[M] ARM_FIRMWARE/application/telemetry_task.c`
- `[M] ARM_FIRMWARE/application/uart_task.c`
- `[A] ARM_FIRMWARE/tests/test_modbus_registers.c`

### 修改说明

1. UART RX 使用 DMA 空闲中断或定时帧间隔识别 RTU 帧，具体取决于 ARM 平台能力。
2. Slave 仅响应地址 `0x01`，串口业务态不主动发送任何帧。
3. 对非法地址/数量/值返回标准 Modbus Exception。
4. 使用双缓冲或短临界区生成只读快照，完成后递增 `SNAPSHOT_SEQ`。
5. 所有倍率转换做饱和和范围检查；传感器无效值使用寄存器表定义的 sentinel。
6. 增加请求数、CRC 错误、非法地址、设备忙和最后请求时间等诊断计数。
7. Slave 请求处理不能阻塞实时控制环，不在 UART 中断内执行 JSON、Flash 或复杂控制。

## 流程 4：ESP UART 总线独占与协议模式切换

### 涉及文件

- `[A] esp32c2_idf/main/uart_bus_owner.c`
- `[A] esp32c2_idf/main/uart_bus_owner.h`
- `[M] esp32c2_idf/main/uart_protocol.c`
- `[M] esp32c2_idf/main/uart_protocol.h`
- `[M] esp32c2_idf/main/arm_ota.c`
- `[M] esp32c2_idf/main/app_main.c`

### 修改说明

1. 将 UART driver 初始化和所有权从 `uart_protocol` 中拆到 `uart_bus_owner`。
2. 旧 parser 只在 `UART_OWNER_LEGACY` 或 `UART_OWNER_ARM_OTA` 下读取。
3. Modbus 调度器在 `UART_OWNER_MODBUS` 下独占 RX/TX。
4. ARM OTA 开始前暂停 Modbus、等待事务完成、flush UART，再切换 OTA owner。
5. OTA 结束/失败后重新探测 ARM Modbus magic；ESP OTA 重启后按正常启动恢复。
6. 所有权切换设超时，失败时拒绝 OTA 或控制，禁止强抢导致半帧混合。

### 目标接口

- `bool uart_bus_acquire(UartBusOwner owner, TickType_t timeout)`
- `void uart_bus_release(UartBusOwner owner)`
- `UartBusOwner uart_bus_current_owner(void)`
- `void uart_bus_reset_transport(void)`

## 流程 5：ESP 寄存器解析与统一 ARM 数据模型

### 涉及文件

- `[A] esp32c2_idf/main/modbus_register_map.c`
- `[A] esp32c2_idf/main/modbus_register_map.h`
- `[A] esp32c2_idf/main/arm_data_model.c`
- `[A] esp32c2_idf/main/arm_data_model.h`
- `[A] esp32c2_idf/test/test_modbus_register_map.c`

### 修改说明

1. 将寄存器转换与业务状态分离，禁止在 Modbus transport 中直接拼 MQTT JSON。
2. 提供 `u16/i16/u32/i32/u64`、倍率和字符串解析助手。
3. 每个数据块解析到类型明确的 `ArmDataModel`，记录块更新时间、有效标志和快照序号。
4. 读取序号前后不一致时整块丢弃，不用部分数据覆盖上次有效值。
5. 越界、sentinel、NaN 等无效来源不得直接发布；按 MQTT 既有契约选择省略、null 或保留上次值。
6. 寄存器表版本 major 不支持时停止业务轮询并报告协议不兼容；minor 增量按 capability 位兼容。

## 流程 6：分级轮询调度与通信健康状态

### 涉及文件

- `[A] esp32c2_idf/main/modbus_poll_scheduler.c`
- `[A] esp32c2_idf/main/modbus_poll_scheduler.h`
- `[M] esp32c2_idf/main/state_machine.c`
- `[M] esp32c2_idf/main/state_machine.h`
- `[M] esp32c2_idf/main/config.h`
- `[M] esp32c2_idf/main/Kconfig.projbuild`

### 修改说明

1. 启动先读取 magic、版本、capability、SN 和固件信息。
2. 快速组每 200 ms：系统状态、告警序号、控制结果。
3. 常规组每 1 s：AC、电池、PV 和逆变器状态。
4. 慢速组每 10～60 s：能量、设备信息、电芯和诊断。
5. 各组错峰执行，每个主循环只发一个事务，避免连续大块读占用 UART。
6. 请求失败重试 2 次；连续 3 个周期失败设置 `arm_online=false` 并指数退避至最大 5 s。
7. 通信恢复后重新读取 magic/版本/静态信息，再恢复快速轮询。
8. 每块维护 freshness deadline；过期数据不得继续伪装为实时数据。
9. 调度器不得在 ESP 主循环中阻塞超过单事务超时，建议独立低栈任务或事件驱动状态机。

## 流程 7：Modbus 数据映射为现有 MQTT 消息

### 涉及文件

- `[A] esp32c2_idf/main/modbus_mqtt_mapper.c`
- `[A] esp32c2_idf/main/modbus_mqtt_mapper.h`
- `[A] esp32c2_idf/test/test_modbus_mqtt_mapper.c`
- `[M] esp32c2_idf/main/mqtt_handler.c`
- `[M] esp32c2_idf/main/state_machine.c`
- `[M] docs/MQTT协议文档.md`

### 修改说明

1. 按现有频率生成 `data/ac`、`data/battery`、`data/pv`、`data/status`、`data/energy`、`data/cells`。
2. 告警序号变化时生成 `data/alarm`，使用 QoS 1。
3. 复用现有 MQTT envelope、SN、timestamp、QoS 和离线缓存入口。
4. 将寄存器缩放值恢复成云端既有单位，禁止把原始寄存器数值直接暴露给后端。
5. 同一快照只发布一次；MQTT 重连不重复采集，但离线缓存按现有规则补发。
6. `arm_online=false` 时发布明确设备侧通信故障，不将 Wi-Fi 在线等同于逆变器在线。

### 目标接口

- `bool modbus_mqtt_publish_due(const ArmDataModel *model, int64_t now_ms)`
- `bool modbus_mqtt_publish_alarm(const ArmDataModel *model)`
- `bool modbus_mqtt_build_topic_payload(...)`

## 流程 8：MQTT 下行控制映射到 Modbus 控制邮箱

### 涉及文件

- `[A] ARM_FIRMWARE/tests/test_modbus_control_mailbox.c`
- `[M] ARM_FIRMWARE/application/control_task.c`
- `[M] ARM_FIRMWARE/communication/modbus_registers.c`
- `[M] esp32c2_idf/main/state_machine.c`
- `[M] esp32c2_idf/main/modbus_register_map.c`

### 修改说明

1. 为每个允许控制的 MQTT `cmd` 定义稳定 `CONTROL_CMD_ID` 和参数布局。
2. ESP 校验 `task_id`、时间戳、权限、参数类型和范围后才写寄存器。
3. ESP 生成单调 `CONTROL_REQ_SEQ`，先写参数和命令，最后写 `CONTROL_COMMIT`。
4. ARM 收到 commit 后复制整个邮箱，立即清 commit，再交给控制任务执行。
5. ARM 用 `CONTROL_RESP_SEQ` 关联结果，返回 state/error/result，不返回不受控长字符串。
6. ESP 等待对应响应序号，映射为现有 `cmd/response`；超时回复 `ARM_TIMEOUT`。
7. 首期限制一个未完成控制命令；新命令到达时返回 busy，禁止覆盖邮箱。
8. ARM 对危险参数进行二次范围校验，即使 ESP 校验通过也不能直接信任。

## 流程 9：ARM OTA 与 Modbus 分时集成

### 涉及文件

- `[M] esp32c2_idf/main/arm_ota.c`
- `[M] esp32c2_idf/main/uart_protocol.c`
- `[M] esp32c2_idf/main/modbus_poll_scheduler.c`
- `[M] ARM_FIRMWARE/bootloader_or_iap/arm_ota_transport.c`
- `[M] ARM_FIRMWARE/application/uart_task.c`

### 修改说明

1. OTA 服务申请 UART owner 成功后才向 ARM 发送 `CMD_ARM_OTA_START`。
2. ARM 接到启动请求后停止 Modbus Slave 响应并进入 IAP/OTA transport。
3. OTA 数据继续使用现有分包、ACK/NACK、MD5/哈希与超时机制。
4. OTA 完成后 ARM 重启，ESP 等待启动窗口，再探测 magic/version。
5. OTA 失败时清 UART、恢复 Modbus Slave；恢复失败则保持 ARM offline 并上报 OTA 错误。
6. OTA 期间暂停 BLE 已获允许；ESP OTA 重启后统一恢复 BLE/AP/MQTT 生命周期。

## 流程 10：模拟器、兼容模式与端到端切换

### 涉及文件

- `[M] tools/arm_simulator/lib/services/uart_service.dart`
- `[A] tools/arm_simulator/lib/protocol/modbus_register_bank.dart`
- `[A] tools/arm_simulator/test/modbus_register_bank_test.dart`
- `[M] esp32c2_idf/main/app_main.c`
- `[M] esp32c2_idf/main/config.h`
- `[M] esp32c2_idf/main/Kconfig.projbuild`
- `[A] tools/tests/modbus_contract.test.mjs`
- `[M] tools/tests/firmware_contract.test.js`

### 修改说明

1. ARM 模拟器新增 Modbus Slave 模式，可配置寄存器、告警跳变、CRC 错误、延迟、超时和异常响应。
2. ESP 增加编译默认值和 NVS 协议选择：`legacy`、`modbus`、`auto`。
3. `auto` 模式启动先探测 Modbus magic；失败后在限定窗口识别旧 `0xAA` 帧，禁止永久双 parser 并行。
4. 联调期同时采集 legacy JSON 与 Modbus 映射结果，在测试工具中对比但只发布一份 MQTT。
5. 字段、频率和命令结果全部对齐后，将生产默认切换为 `modbus`。
6. 旧协议保留至少一个发布周期，仅用于现场兼容和 ARM OTA，不继续增加业务字段。

## 流程 11：内存、稳定性和硬件在环验收

### 涉及文件

- `[M] esp32c2_idf/main/app_main.c`
- `[A] tools/tests/modbus_contract.test.mjs`
- `[A] docs/ARM_ESP32_Modbus_Register_Map.md`

### 修改说明

1. 记录无 MQTT、MQTT TLS 在线、BLE 连接、AP HTTP 活跃四种组合的 free heap、largest block 和 minimum heap。
2. Modbus 正常运行不得使用按帧动态分配；连续运行 24 h 后 minimum heap 不应持续下降。
3. 注入 CRC 错误、丢帧、100 ms 延迟、ARM 重启、连续异常响应和 UART 垃圾字节。
4. 验证 ARM offline/online 状态切换、数据过期、不重复告警和通信恢复。
5. 同时验证 MQTT 断网缓存、恢复补发和 Modbus 轮询不受云端状态影响。
6. 在 ARM OTA、本地 ESP OTA、远程 ESP OTA 期间验证 UART owner 和 BLE 暂停/恢复。

---

## 4. 每个文件的落地改动摘要（汇总视图）

1. `[A] esp32c2_idf/main/modbus_rtu_master.c/.h`
   - 提供固定缓冲、同步事务、CRC、功能码和错误分类，不包含业务字段。
2. `[A] esp32c2_idf/main/modbus_register_map.c/.h`
   - 固化寄存器地址、类型、倍率、范围和命令邮箱布局。
3. `[A] esp32c2_idf/main/arm_data_model.c/.h`
   - 保存解析后的类型化数据、有效性、更新时间和快照序号。
4. `[A] esp32c2_idf/main/modbus_poll_scheduler.c/.h`
   - 管理快速/常规/慢速轮询、重试、退避、freshness 和 ARM 在线状态。
5. `[A] esp32c2_idf/main/modbus_mqtt_mapper.c/.h`
   - 将 `ArmDataModel` 转换为现有 MQTT Topic/Payload。
6. `[A] esp32c2_idf/main/uart_bus_owner.c/.h`
   - 保证 Modbus、Legacy 和 ARM OTA 三种协议互斥拥有 UART。
7. `[M] esp32c2_idf/main/uart_protocol.c/.h`
   - 从默认业务 parser 收缩为兼容/OTA transport，并接入 UART owner。
8. `[M] esp32c2_idf/main/app_main.c`
   - 初始化 UART owner、Modbus transport、scheduler 和协议探测；保留启动内存日志。
9. `[M] esp32c2_idf/main/state_machine.c/.h`
   - 去除 ARM 主动 `CMD_PUBLISH` 作为主数据入口，接入轮询数据和控制邮箱结果。
10. `[M] esp32c2_idf/main/arm_ota.c`
    - 在 OTA 前后申请/释放 UART owner，恢复 Modbus 探测。
11. `[M] esp32c2_idf/main/mqtt_handler.c`
    - 保留 MQTT 连接和发布能力，为 mapper 提供稳定发布入口。
12. `[M] esp32c2_idf/main/config.h`、`Kconfig.projbuild`
    - 增加 Slave 地址、超时、重试、轮询周期和协议模式配置。
13. `[M] esp32c2_idf/main/CMakeLists.txt`
    - 注册新增 Modbus、数据模型、调度器和 mapper 源文件。
14. `[A] esp32c2_idf/test/test_modbus_*.c`
    - 覆盖 codec、寄存器解析、快照一致性和 MQTT 映射。
15. `[A] tools/tests/modbus_contract.test.mjs`
    - 检查 ESP/ARM/文档地址和字段契约不漂移。
16. `[M] tools/arm_simulator/lib/services/uart_service.dart`
    - 增加 Modbus Slave transport 和错误注入入口。
17. `[A] tools/arm_simulator/lib/protocol/modbus_rtu_codec.dart`
    - 与 ESP 共享向量的 Dart RTU codec。
18. `[A] tools/arm_simulator/lib/protocol/modbus_register_bank.dart`
    - 提供模拟寄存器、快照序号、告警和控制邮箱。
19. `[A] docs/ARM_ESP32_Modbus_Register_Map.md`
    - 成为 ARM/ESP 唯一寄存器契约源，包含版本策略和完整字段表。
20. `[M] docs/ARM_ESP32_UART_Protocol.md`
    - 标记业务帧迁移状态，保留 Legacy/OTA 协议说明。
21. `[M] docs/MQTT协议文档.md`
    - 将数据来源由 ARM JSON 透传更新为 ESP Modbus 映射，但保持外部消息结构。
22. `[A] ARM_FIRMWARE/communication/modbus_*.c/.h`
    - 实现 Slave、寄存器访问、异常响应、诊断和一致性快照。
23. `[M] ARM_FIRMWARE/application/telemetry_task.c`
    - 将实时数据安全提交到对外快照。
24. `[M] ARM_FIRMWARE/application/control_task.c`
    - 消费控制邮箱并发布带序号的稳定结果。
25. `[M] ARM_FIRMWARE/application/uart_task.c`
    - 正常态运行 Modbus Slave，OTA 态切换专用 transport。

---

## 5. 测试与验收

### 5.1 测试文件

1. `[A] esp32c2_idf/test/test_modbus_rtu_master.c`
2. `[A] esp32c2_idf/test/test_modbus_register_map.c`
3. `[A] esp32c2_idf/test/test_modbus_mqtt_mapper.c`
4. `[A] tools/tests/modbus_contract.test.mjs`
5. `[M] tools/tests/firmware_contract.test.js`
6. `[A] tools/arm_simulator/test/modbus_rtu_codec_test.dart`
7. `[A] tools/arm_simulator/test/modbus_register_bank_test.dart`
8. `[A] ARM_FIRMWARE/tests/test_modbus_registers.c`
9. `[A] ARM_FIRMWARE/tests/test_modbus_control_mailbox.c`

### 5.2 核心测试点

1. 标准 CRC16 向量、错误 CRC、最短帧、最大帧和垃圾前缀。
2. `0x03/0x04/0x06/0x10` 正常响应及 Exception Response。
3. 16/32/64 位、signed、倍率、字符串和 sentinel 解析。
4. 快照序号变化时整批重试，不发布撕裂数据。
5. 轮询频率、错峰、超时、2 次重试、3 周期离线和恢复探测。
6. 告警序号变化立即发布，位图不变不重复发布，快速告警不漏报。
7. MQTT Payload 与迁移前固定向量逐字段一致。
8. 控制邮箱参数先写、commit 最后写，ARM 不执行半写请求。
9. 请求序号与 MQTT `task_id` 正确关联，超时/重启不误报成功。
10. ARM OTA 获取 UART 独占后 Modbus 停止；OTA 完成后恢复探测。
11. MQTT 离线时 Modbus 正常采集，缓存恢复后按 PUBACK 删除。
12. BLE、AP、MQTT TLS、Modbus 同时运行时无持续堆下降。

### 5.3 执行顺序与命令

1. 运行固件协议合同测试：

   `node --test tools/tests/firmware_contract.test.js tools/tests/modbus_contract.test.mjs tools/tests/esp32c2_ota_contract.test.mjs tools/tests/ota_signature.test.mjs`

2. 运行 ARM 模拟器协议测试：

   `flutter test test/modbus_rtu_codec_test.dart test/modbus_register_bank_test.dart`

   工作目录：`tools/arm_simulator`

3. 构建 ESP32-C2：

   `idf.py build`

   工作目录：`esp32c2_idf`

4. 运行 ESP-IDF Unity 测试固件（测试工程接入后）：

   `idf.py -T test_modbus_rtu_master -T test_modbus_register_map -T test_modbus_mqtt_mapper build flash monitor`

5. 运行 ARM 工程测试：

   使用 Gate 0 确认后的 ARM 构建/测试命令；未确认前不得在计划中伪造工具链命令。

6. 烧录 ESP32-C2 并监视：

   `idf.py -p COM12 flash monitor`

7. 硬件在环运行至少 24 h，记录：

   - Modbus 请求/成功/CRC/超时/异常计数。
   - ARM online/offline 切换次数。
   - MQTT 发布与离线缓存积压量。
   - free heap、minimum heap、largest free block。
   - 控制命令成功率与 P95 响应时间。

### 5.4 验收标准

1. 连续 24 h 无崩溃、看门狗复位、堆持续下降或 UART 死锁。
2. 正常链路 Modbus 请求成功率 ≥ 99.9%，CRC 错误为 0；注入故障时能按设计恢复。
3. 主要遥测端到端 MQTT 延迟 P95 ≤ 2 s；关键状态/告警 P95 ≤ 500 ms。
4. MQTT Payload 与现有云端契约兼容，后端无需修改即可解析。
5. 命令不重复执行、不覆盖未完成请求，结果与 `task_id` 一一对应。
6. ARM 重启后 ESP 在 10 s 内恢复协议探测与数据上报。
7. MQTT 断网不影响 Modbus 采集；恢复后缓存数据按 QoS/PUBACK 规则补发。
8. ARM OTA 与 ESP OTA 均可完成，OTA 期间不存在 Modbus/OTA 混帧。
9. BLE + AP + MQTT TLS + Modbus 正常运行时保留的 free heap 和 largest block 达到后续内存预算要求；建议验收下限均不低于 10 KiB。

---

## 6. 实施顺序建议

1. 完成 Gate 0，拿到 ARM 工程并冻结寄存器表 1.0。
2. 测试优先实现 C/Dart Modbus codec，使用共享向量完成红→绿。
3. ARM 实现只读 Slave、magic/version 和一小块 AC 测试寄存器。
4. ESP 实现 Master 和 UART owner，只读取测试块，不接 MQTT。
5. 接入 ARM 模拟器与真实 ARM，完成错误注入和快照一致性验证。
6. 扩展全部只读遥测寄存器，建立 `ArmDataModel`。
7. 实现 MQTT mapper，逐 Topic 与旧链路做固定向量对比。
8. 实现告警事件序号和 QoS 1 上报。
9. 实现控制邮箱，先上低风险命令，再开放参数设置和危险控制。
10. 集成 ARM OTA UART 独占与恢复探测。
11. 启用 `auto`/双协议联调，对比 legacy 与 Modbus 数据。
12. 完成 24 h 硬件在环、MQTT 断网、ARM 重启、BLE/AP 和 OTA 压力测试。
13. 验收通过后将生产默认协议切换为 `modbus`，旧协议冻结为兼容/OTA用途。

---

## 7. 主要风险与决策点

1. ARM 不可修改：无法按推荐方案实施，只能增加 ESP 对 ARM 现有协议的适配，需重新设计。
2. ARM 已有 Modbus 表：应复用并建立适配层，不能直接采用本文建议地址。
3. 实时控制任务不允许复制大快照：改为分组双缓冲，每组独立序号。
4. RS485 而非 TTL：需要增加 DE/RE 控制、发送完成等待和总线释放时序测试。
5. 告警只能提供当前位图：可能漏掉短暂事件，必须让 ARM 增加事件序号或小型事件 FIFO。
6. MQTT 命令允许并发：首期邮箱只能单并发，云端或 ESP 必须明确返回 busy；后续再设计有界命令队列。
7. BLE 常驻会进一步压缩 RAM：Modbus transport 必须固定小缓冲，不能引入大队列或每帧动态分配。
8. OTA transport 与 Modbus 使用同一 UART：所有权切换是上线前强制验收项，任何混帧都不可接受。
9. 寄存器表版本发布后变更字段含义：属于破坏性协议变更，必须升级 major，ESP 对未知 major 拒绝业务运行。

