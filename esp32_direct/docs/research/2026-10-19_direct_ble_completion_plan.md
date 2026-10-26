# direct 固件手机蓝牙完善研究与实施规划

> 2026-10-19 实施更新：用户确认保持连接即用，已启动 P0。本文其余章节保留研究时的现状和 P1–P3 规划，不能作为当前交付清单；P0 实施与验证记录见 `../BLE_P0_20261019.md`。尚未刷机、安装或发布。

## 0. 文件树变更清单

以下是后续实施的建议范围，不表示本轮已修改业务代码。`[A]` 新增，`[M]` 修改；本方案不需要删除文件。

```text
F = D:/CS_INV_WIFI/esp32_arm_replacement
  [A] docs/research/2026-10-19_direct_ble_completion_plan.md  本研究文档
  [M] components/ble_ct/ble_ct.c
  [M] components/ble_ct/ble_ct.h
  [M] components/ble_ct/ble_ct_cmd.c
  [M] components/ble_ct/ble_ct_cmd.h
  [M] components/ble_ct/ble_ct_telemetry.c
  [M] components/ble_ct/ble_ct_telemetry.h
  [A] components/ble_ct/ble_ct_frames.c
  [A] components/ble_ct/ble_ct_frames.h
  [M] components/ble_ct/CMakeLists.txt
  [M] components/ble_prov/ble_prov.c
  [M] main/app/app_main.c
  [M] main/telemetry/telemetry.c
  [M] main/telemetry/cmd_handler.c
  [M] main/direct/direct_poll.c
  [M] main/direct/direct_link.h
  [M] main/direct/direct_ui.c
  [A] main/direct/direct_param_schema.c
  [A] main/direct/direct_param_schema.h
  [M] main/CMakeLists.txt
  [A] tests/ble_info_contract_test.py
  [A] tests/ble_frames_test.py
  [A] tests/ble_live_snapshot_test.py
  [A] tests/ble_control_flow_test.py
  [M] tests/controller_command_test.py
  [M] tests/run_production_host_tests.ps1

A = D:/CS_APP_PROJECT/cs_inv_monitor-develop/cs_inv_monitor-develop/inv_app
  [M] lib/core/services/ble/ble_adapter.dart
  [M] lib/core/services/ble/ble_device_manager.dart
  [M] lib/core/services/ble/ble_direct_service.dart
  [M] lib/core/services/ble/ble_polling_service.dart
  [M] lib/core/services/ble/device_live_snapshot.dart
  [M] lib/core/services/ble/device_live_data_service.dart
  [M] lib/core/services/service_locator.dart
  [M] lib/core/services/realtime_data_service.dart
  [A] lib/features/device/domain/services/device_config_access.dart
  [A] lib/features/device/data/datasources/ble_device_config_source.dart
  [A] lib/features/device/data/datasources/cloud_device_config_source.dart
  [M] lib/features/device/domain/entities/config_schema.dart
  [M] lib/features/device/presentation/pages/device_realtime_page.dart
  [M] lib/features/device/presentation/pages/device_settings_page.dart
  [M] lib/features/device/presentation/pages/device_settings_category_page.dart
  [M] lib/features/device/presentation/pages/device_settings_advanced_page.dart
  [M] lib/features/device/presentation/pages/device_control_page.dart
  [M] lib/features/device/presentation/pages/add_device_page.dart
  [M] lib/features/profile/presentation/pages/offline_mode_settings_page.dart
  [M] lib/core/services/offline/offline_op_log_store.dart
  [M] lib/l10n/app_zh.dart
  [M] lib/l10n/app_en.dart
  [M] test/ble/ble_device_session_test.dart
  [M] test/ble/ble_direct_service_test.dart
  [M] test/ble/ble_polling_service_test.dart
  [M] test/ble/device_live_data_service_test.dart
  [M] test/ble_frame_reassembler_test.dart
  [A] test/ble/direct_config_access_test.dart
  [A] test/features/device/ble_device_pages_test.dart
```

这份清单覆盖完整目标；第一阶段只处理 INFO、传输和来源模型涉及的文件。只有进入对应阶段，才修改后续页面与配置模块。新模块名是规划建议，实施时如已有同职责实现，应直接复用。

## 1. 结论与范围

日期：2026-10-19。状态：可评审的研究与实施草案；本地连接方式已由用户确认。研究基线为 F 当前工作区 `0.6.34-provisioning-dev`，A 当前源码版本 `1.1.12+40`；均不代表设备或手机正在运行的版本。

建议复用现有 BLE 会话、App 实时数据汇聚服务、设置控件和 OTA 独占机制，分阶段完成手机使用流程：发现设备 → 核对 SN → 连接 → 查看信息/实时数据 → 读取参数 → 修改并追踪实际结果 → 断线恢复。

首期目标是完整支持单台 direct 设备的常用读取和设置。一个设备当前只允许一个手机连接；手机可以记忆多台设备，列表分别显示连接状态。多设备并发控制、蓝牙后台网关、BMS 校准和工厂身份修改不纳入首期。

本轮仅新增此规划文档。未编译、刷机、部署或验证真实手机通信。现有工作区包含大量前序改动，实施时必须在这些改动上明确基线，不使用干净 HEAD 替代当前有效成果。

## 2. 当前证据与必须先修复的问题

### 2.1 可以继续使用的基础

- F `main/app/app_main.c:515` 已启动 CT、注入命令执行器并注册配网/OTA 服务，BLE 常驻。
- F `components/ble_ct/ble_ct.c:106` 已有 INFO、TELEMETRY、COMMAND、CMD_RESULT；无需新建另一套蓝牙服务。
- F `main/direct/direct_dsp_params.c:24` 的 `sane()` 与 `direct_dsp_params_set()` 已执行电压、频率、枚举和部分跨字段约束。上一轮将范围校验概括为仅有 uint16 边界不够准确；本次应统一现有规则及错误反馈。
- A `ble_device_manager.dart:315` 已实现会话、订阅、命令关联与重连；`BleOtaLease` 已实现 OTA 独占。
- A 已有 `DeviceLiveSnapshot`、`DeviceLiveDataService` 与单元测试，具备 SN 隔离、采样时间和连接代次模型；当前尚未注册并接入生产数据路径。
- A 已有 `ConfigParamSchema` 与设置控件，现有设置页通过 HTTP 取 schema、取值和写入。可抽出数据访问接口，复用页面与控件。

### 2.2 阻断与误判风险

| 优先级 | 已核实的源码事实 | 对手机使用的影响 | 实施要求 |
|---|---|---|---|
| P0 | F `ble_ct.c:75` 用 512 字节缓冲编码 INFO；当前版本、16 位 SN、32 位 session 的完整 JSON 长 517 个 ASCII 字节，另需结尾 NUL | `n >= sizeof(buf)` 直接返回 ATT 错误；A 随后按协议识别失败处理，控制请求缺 session | 首先修正 INFO 体积并运行真实编码测试 |
| P0 | F `direct_poll.c:792` 明确记录“saved on ESP and three sync frames submitted; DSP application unconfirmed” | 当前回调成功并非 DSP 已采用参数 | 保存、发送、观察确认分开报告 |
| P0 | F `cmd_handler.c:258` 回读 `direct_config_get_wire()`，后者优先读取持久期望值 | 写入自己保存的值，再读取它，可能被误判为真实 DSP 确认 | 独立保留 desired 与 observed，确认必须引用写入后的设备观测 |
| P0 | F `ble_ct_cmd.c:329` 将 `power_on/off` 映射为 `output_priority=0/1`；F 持久参数要求该模式为 1..4 | 开机值被拒绝；关机别名改变优先级，不证明实际关闭输出 | 首期禁用这两个别名；取得真实控制指令和状态证据后另行开放 |
| P0 | F `ble_ct_telemetry.c:13` 固定每帧 509 字节正文，另加一字节帧头 | 在 MTU=512 时已超过 `512-3` 的通知载荷；较小 MTU 更无法可靠传输 | 分片正文最多 `actual_mtu-3-header_size` |
| P1 | F 遥测快照只在已订阅且有连接时更新；刷新跟随云心跳，默认约 30 秒 | 刚连接可能读到 `{}` 或前一连接旧数据；提高 App 轮询频率也无法得到新采样 | 采样快照独立更新，通知单独节流 |
| P1 | F BLE 遥测使用发送时钟作为 `updated_at`，未校时写 1970；PF、运行时长为固定 0，状态固定 offgrid | 手机无法可靠区分未知、实测零、真正采样时间和状态 | 真实来源、有效性、采样序号与时间质量显式传递 |
| P1 | F BLE 遥测把带符号电流整数部分和小数部分分别格式化 | -0.5A 这类值可能变成 +0.5A | 统一有符号定点编码，增加边界向量 |
| P1 | F INFO 仍为占位硬件/schema，固定列出 system_controller 等升级模块 | 信息不完整，也可能展示并不存在的 ARM 模块 | 精简 INFO + 分页详细信息，能力从真实实现产生 |
| P1 | A 设备直连开关只保存 MAC；自动连接仍依赖旧 device_key；当前云绑定不生成该 key | 新版 direct 用户可能“打开开关但没有实际连接” | 显式连接/断开；连接记忆与云归属分别管理 |
| P1 | A `BlePollingService.telemetry` 与 session 通知流未接入实时页面；设置页仍直接使用 Dio | 可扫描/连接不等于能在业务页面查看和设置 | 将生产者与现有数据服务、页面连通 |
| P1 | F 写操作状态跨 BLE 任务和主循环，主要按参数名关联，缺完整操作状态查询 | 断线、晚到回调或回执丢失后，App 难以判断是否已执行 | 操作 ID、有限生命周期、可查询结果、连接代次隔离 |

517 字节是本轮依据当前编码字段进行的确定性长度计算，不是手机实测。现有 host 日志中“controller readback”等 PASS 使用模拟存储与传输，不能证明 DSP 应用确认；新的测试必须覆盖真实 direct 调用链。

### 2.3 参数数量不能直接等同于支持能力

当前同时存在 LCD/协议的 84 个索引、历史云端 42 个常用键、命令别名表、持久参数包支持集合和 0x25 可观测集合。它们不是同一个集合。

例如电池容量在命令映射中存在，但当前持久包 getter 没有对应支持；电池类型 setter 只允许保持当前类型，切换需要完整电池配置。不能把“映射表中有名称”作为手机可编辑的依据。

实施前输出逐参数矩阵：`param_key / index / 工程单位 / 原始单位 / 变换 / 范围 / 步进 / 枚举 / 可读 / 可持久保存 / DSP 可确认 / 联动条件`。只开放已核对的行。

## 3. 方案取舍与建议

| 方案 | 实施方式 | 成本与适用性 |
|---|---|---|
| 推荐：补齐现有链路并复用页面 | 修传输与数据语义，接入现有实时汇聚与设置控件，本地连接按当前体验使用 | 固件和 App 都需调整；投入能够直接服务完整产品 |
| 独立做一个蓝牙工具页 | 新建只服务 BLE 的信息、数据和参数页面 | 初期较快，但会形成第二套参数 UI、校验和来源逻辑，后续维护成本更高 |
| 一次加入云签发的离线管理凭据 | 在第一方案上增加服务端授权、凭据轮换、离线有效期和撤权 | 适合明确的组织管理需求；需要后端和量产身份体系，不作为首期依赖 |

用户已于 2026-10-19 确认“保留连接即用，优先补齐功能”。本期蓝牙查看和参数设置不增加 PIN、屏幕限时确认、云端归属或离线授权凭据前置；优先完成可靠读写、设备端参数约束、真实结果确认和操作记录。云端账号绑定继续服务云端业务，不作为本地 BLE 操作条件。上表其他授权方案仅保留为取舍记录，不列入本期实施依赖。

按已确认的连接方式，本期不恢复 PIN/HMAC 门槛。连接即用仍须执行 SN 核对、参数能力与范围检查、修订冲突检测及 OTA 互斥。配网密码应只写不读，提交后及时清除会话中的临时明文；这是配网数据处理修正，不改变用户连接步骤。

## 4. 产品流程与界面行为

- 附近设备：显示名称、信号、完整 SN 确认入口；点击连接后先读 INFO，扫描广播名仅用于发现。
- 设备详情：沿用现有实时/设置/升级入口，顶部显示“蓝牙直连 / 云端”、蓝牙连接和 DSP/BMS 在线状态。蓝牙连上但 DSP 离线时明确展示各自状态。
- 实时页：负载、电网、PV、电池、故障与温度；BMS 摘要和单体详情按支持能力展示。缺失为 `—`，真实零为 `0`，过期末值继续显示并标注“数据已过期”。
- 设置页：读取当前保存值与设备观测值，未改动前以可信来源显示；编辑保留草稿，刷新不能覆盖用户未提交的修改。
- 应用设置：展示改动项与单位，按顺序逐项执行，逐项显示结果；发生失败停止后续发送并保留未提交草稿。无批量原子能力时不提示“全部已生效”。
- 断线：保留末值，关闭写入口，标记未完成操作为结果待确认；重连先核对 SN，再查询原操作结果与新快照。
- OTA：复用升级入口，升级期间暂停实时推送和参数写；结束后重新取 INFO、参数及实时数据。
- 云端与本地同时可用：页面明确采用的数据来源；单次写入固定一个通道，超时后不自动换通道再次写入。

## 流程 1：修通 INFO 与传输边界

### 涉及文件

F `ble_ct.[ch]`、`ble_ct_frames.[ch]`、`ble_ct_cmd.[ch]`、`ble_ct_telemetry.[ch]`、`components/ble_ct/CMakeLists.txt`、`ble_prov.c`；A `ble_adapter.dart`、`ble_device_manager.dart`。

### 修改说明

1. INFO 只保留握手所需字段：SN、型号、运行固件、session、协议版本、schema 版本与能力摘要；候选目标不超过 480 字节。详情通过后续分页查询，不能直接塞入约 4KB 的 factory INFO。
2. `max_message_bytes` 等预算由真实分配决定。初始候选为完整逻辑消息不超过 512 字节，接收文本缓冲至少 513 字节含 NUL；最终以编码最坏长度和内存验证冻结。480/512 是设计目标，不是当前通过的能力。
3. 延续现有服务 UUID 和 v2 信封。新行为用明确 capability 协商，候选为 `direct_live_v1`、`direct_config_v1`、`direct_control_v1`、`result_query_v1`、`framing_v1`；没有能力时只提供设备实际支持的旧读功能。
4. 新客户端读 INFO 后发送 `open_session` 协商需要的能力，设备确认前保持旧格式。若 INFO 已声明 framing_v1，接收端从此可接受带帧头的握手请求，open_session 回执按请求指定格式返回；否则先使用完整 JSON 长写。这样小 MTU 的第一次握手不依赖一个尚未完成的协商。新增接口和状态的完整向量先冻结到契约；不得在同一版本下让旧 App 无提示地解析新格式。
5. 通知分片正文上限为 `MTU-3-1`；MTU=23/185/247/256/512 对应 19/181/243/252/508 字节。CMD_RESULT 同样支持协商后的分帧，不能只修遥测。
6. 旧的 8 帧上限不适合小 MTU：512 字节正文在 MTU=23 需要 27 帧。新分帧候选上限 64 帧，同时独立限制完整消息为 512 字节、一个在途重组和重组超时；每特征串行发一整条消息。
7. COMMAND 在支持的平台使用有响应长写；不支持长写时使用已协商的应用分帧。无可用路径则返回明确的不支持，不截断 JSON。
8. INFO/快照的长读必须读取一个固定版本，避免 Read Blob 期间后台刷新导致拼接两代 JSON；快照写入/读取使用短临界区或双缓冲。
9. 失败包括缓冲不足、缺帧、序号跳变、超时、断连、NimBLE 重启和发送队列满；结果都可观察。禁止部分 JSON、静默裁剪或“写成功即业务成功”。
10. INFO 中不存在 ARM 时不公布 ARM 升级能力。DSP/BMS 的支持、当前可用性和是否完成实机验收分别表达；本阶段不顺带改变 OTA manifest 验签或兼容政策。

## 流程 2：建立真实实时快照

### 涉及文件

F `main/telemetry/telemetry.c`、`ble_ct_telemetry.[ch]`、`direct_poll.c`、`direct_link.h`；A `device_live_snapshot.dart`、`device_live_data_service.dart`、`ble_device_manager.dart`、`ble_polling_service.dart`。

### 修改说明

1. 新采样/状态变化时更新 BLE 缓存，独立于是否连接、是否订阅及 MQTT 发布周期。无线发送节流与采样刷新分离。
2. 首次进入实时页立即读一个快照并启动订阅。前台目标为最多每秒一次推送，事件变化触发，未变化不空刷；普通后台停止高频订阅，断连停止读取。
3. 快照包含 `device_sn / boot_id / sample_seq / source_uptime_ms / sample_uptime_ms / age_ms / sampled_at / time_quality / quality`。上电无 UTC 时 `sampled_at=null`，使用同一启动的单调时钟计算年龄，不能编造 1970 或手机采样时间。
4. 现有 `DeviceLiveSnapshot.ble` 必须传有效 DateTime，因此扩展为允许缺 UTC；新鲜度只在缺 UTC 时使用设备单调年龄加 App 接收后的单调时间估计。校时跳变、换 boot_id、跨来源比较都需专门处理，不能直接把“刚收到”解释成“刚采样”。
5. DSP 03 状态、04 测量、BMS 的采集序号和年龄分别保留。某一组离线不能隐藏其余组，也不能用新状态帧让旧测量变新。小报文用分组快照或分页，不把全部单体数组塞进一个核心消息。
6. 本地显示过期阈值先沿用已知固件采样有效性（DSP 当前 2000ms），并与实际 DSP 时序对照后冻结；App 现有三分钟默认不可直接当作本地实时标准。
7. PF、运行时长及未取得的运行模式使用 null/未知；负电流、负温度、零、最大值与能量字段逐项核对单位。PV 归零等展示策略标明派生性质，不改写原始测量。
8. 快照接入现有汇聚服务，同时丢弃旧 session/旧连接代次的异步数据。BLE 与云数据只按兼容的采样时间、序号和新鲜度选择；来源不可比较时保持明确来源，避免云端缓存覆盖正在显示的新本地数据。

## 流程 3：连接流程和页面接线

### 涉及文件

A `ble_direct_service.dart`、`ble_device_manager.dart`、`ble_polling_service.dart`、`service_locator.dart`、`realtime_data_service.dart`、`device_live_data_service.dart`、`device_realtime_page.dart`、`add_device_page.dart`、`offline_mode_settings_page.dart`、`app_zh.dart`、`app_en.dart`；F `ble_ct.c`、`ble_prov.c`。

### 修改说明

1. 用户打开单设备直连开关时实际执行连接，关闭时断开；连接失败不留下“已连接”假象。状态绑定 session 的 connecting/ready/disconnected，不以 Map 中存在 session 判断就绪。
2. 自动连接使用“已选择记忆的 SN 和设备标识”，不要求当前固件不存在的 device_key。旧固件原有认证能力按协议识别保留，新旧流程分支不能互相阻塞。
3. `DeviceLiveDataService` 在服务注册处创建，BLE 通知/轮询及现有云实时数据都进入同一生产接线，页面按 SN 订阅它；App 不把收到的 BLE 遥测再次上传成云采样。
4. 扫描、配网、绑定、读参数和 OTA 共用连接协调，避免两个扫描任务互相停止；记忆地址只用于发现，重连后仍核对 INFO 的完整 SN。标识变化时允许重新发现，不只依赖 MAC。
5. 状态至少包含：蓝牙未开启、权限未授予、连接中、已连接、数据过期、设备内部通信异常、升级中、重连中。允许取消与手动重试。
6. 前后台切换、页面销毁、账号退出和蓝牙关闭时清理订阅及当前连接代次；离开页面后晚到事件不能更新已销毁界面。

## 流程 4：统一参数能力和 schema

### 涉及文件

F `direct_param_schema.[ch]`、`direct_ui.c`、`direct_poll.c`、`direct_link.h`、`cmd_handler.c`、`main/CMakeLists.txt`、`ble_ct_cmd.[ch]`；A `config_schema.dart`、`device_config_access.dart`、`ble_device_config_source.dart`、`cloud_device_config_source.dart`。

### 修改说明

1. 从现有持久参数 getter/setter、UI 范围、collector 转换和真实 DSP 观测建立参数描述表；已有验证函数仍是最后防线，不能只靠 App 限制滑块。
2. 显式提供 `readable / writable / supported / reason / storage_owner / confirmation_method`。有效支持取映射、当前硬件、当前电池配置和实际存储能力的交集。
3. 单位统一为工程单位。原始缩放、枚举转换只在固件边界完成；A 的 schema.scale 不二次乘除。拒绝不符合步进的小数，避免强制转 uint16 静默截断。
4. 新增分页 `get_schema`、`get_params` 与 `query_operation`；分页同时携带 schema_hash、revision 和 cursor，回复不超过协商上限。中途 revision 变化则重启本次读取，不拼接两代参数。
5. 不依赖互联网获取离线 schema。App 可缓存已核对 schema，但当前设备版本/哈希不匹配时只能读取，不使用旧范围盲写。文案翻译由 App 提供，固件传稳定 key。
6. 首批建议评估：输出优先级、充电优先级、总充电电流、市电充电电流、回市电电压、回电池电压、蜂鸣器、背光。逐项证明后开放；输出电压/频率列为需要明确确认的后续常用项。
7. 电池类型切换、整组充电曲线、并机、均衡、发电机和插座控制按实际能力逐组补齐。84 个索引均有明确支持状态，不要求首期全部可写。BMS 校准与 MOS 控制不出现在普通设置页。

## 流程 5：把保存与设备采用分开确认

### 涉及文件

F `ble_ct_cmd.[ch]`、`cmd_handler.c`、`direct_poll.c`、`direct_link.h`、`main/app/app_main.c`；A `ble_device_manager.dart`、`ble_device_config_source.dart`、`device_config_access.dart`、设置页与控制页。

### 修改说明

1. 使用统一操作记录贯穿 BLE 请求、主任务、持久化和 UART。记录 `operation_id / command_id / param_key / desired_revision / saved / submitted / observed / error / deadline`。
2. 状态含义固定如下；拟新增的完成级别必须通过能力协商，不复用旧 `applied` 语义欺骗旧 App。

| 对外结果/阶段 | 证据要求 | 手机文案 |
|---|---|---|
| accepted | 通过当前值、版本、范围及联动检查，进入执行 | 正在设置 |
| executing + confirmation_stage=saved | ESP 持久化成功，并能恢复该 revision | 已保存到控制器 |
| executing + confirmation_stage=submitted | 对应 revision 的配置已提交串口 | 已发送，等待设备确认 |
| saved（新增终态） | 已保存，但当前对端无法提供采用证据，或等待观察的期限已到；同时返回 submitted 和未确认原因 | 已保存，生效未确认 |
| applied | 写入之后收到真实 DSP 观测/应答，参数和期望一致；本地参数则由实际消费模块确认 | 已生效 |
| failed / rejected | 有明确保存失败、拒绝、范围冲突、硬件错误等证据 | 失败原因与可操作提示 |
| unknown（App 认知状态） | 超时、断连或重启导致结果无法证明 | 结果待确认，可查询 |

3. 推荐线格式固定为：保留旧 accepted/executing/applied/rejected/failed；只新增终态 saved，中间进展用 confirmation_stage 表达；所有结果包含 saved/submitted/observed 布尔证据以及目标 revision。仅协商 direct_control_v1 后允许使用新增终态。旧客户端请求需要确认但设备无法提供的写入，在入队前明确拒绝并提示升级 App；不能仍返回虚假 applied，也不能永远停留 accepted。
4. 可观测参数：仅使用写入后、对应目标和允许时序的真实 0x25/已证实应答确认；没有 0x25 的 DSP 固件不能被强制认定为支持回读。部分运行电流上限是动态限制，不能直接覆盖用户保存上限。
5. 没有观测能力时，执行以 saved 终态结束并释放队列，后续可查询；有观测能力但等待超时则以 saved + CONFIRMATION_TIMEOUT 明确说明未确认。已经持久保存后发生 UART 错误，返回 failed 且 saved=true，提示保存值仍存在。不能让一个操作永久阻塞所有写入，不能把观察超时当成修改从未发生。App 自身因断连超时而未取得设备结果时才保持 unknown。
6. 写入后更新 observed 必须有独立来源，禁止通过 `direct_config_get_wire()` 读 desired 充当确认。保存失败、串口失败、DSP 拒绝及观测不符使用不同错误码；当前 -2 不能笼统解释为回读不一致。
7. 同一 command_id + 相同规范化业务内容返回原结果；不同业务内容拒绝。业务摘要不包含每次变化的 transport session。结果缓存有界，查询携带原 operation/command；新 session 按当前访问策略查原结果，不重新执行旧意图。设备重启缓存丢失时返回未知，读值相同也不能证明 exactly-once。
8. 单个设备写串行。屏幕、BLE、MQTT 通过同一校验和写入入口，并校验 expected_revision；当前草稿基线已被另一通道更新时要求用户重新核对。
9. BLE 回调只做有界解析和入队，NVS 写入留在主任务，UART owner 不承担 JSON 编码和网络等待。两个任务间操作状态用队列/短锁关联，避免回调抢在 accepted 缓存建立前完成造成最终结果被覆盖。
10. 首期移除手机对 power_on/off 别名的使用；固件收到这些旧别名应明确拒绝。恢复开关机必须取得真实 DSP 控制命令、运行前提与独立输出状态确认。

## 流程 6：复用设置页面、接入操作记录与 OTA

### 涉及文件

A `device_config_access.dart`、两个 config source、三个设置页面、`device_control_page.dart`、`offline_op_log_store.dart`、`app_zh.dart`、`app_en.dart`、`service_locator.dart`；F `ble_ct_cmd.c`、`main/app/app_main.c`。

### 修改说明

1. 配置访问接口提供 schema、当前参数、提交修改、查询操作四项业务能力；cloud source 包装现有远程实现，BLE source 复用现有 session。继续使用 ConfigParamSchema 和参数控件，不重建第二套产品界面。
2. 显示 saved/desired 和 observed 的区别，值旁展示确认状态；刷新只替换未修改的字段。逐项提交报告明确展示“第几项成功、哪项失败、剩余未发送”。
3. 操作发送前保存本地意图和 operation_id，结果更新同一条记录，记录通道、设备 SN、当时账号上下文、前后值及证据级别。复用现有离线日志同步；它只同步记录，不重放设备命令。同步权限仍由后端校验。
4. 当前日志表升级需兼容既有 pending/synced 记录。若需补充账号归属，复用已有离线同步约定并核对后端接口；首期不为此增加新的服务端消息通道。
5. OTA 沿用 `BleOtaLease`。采集页与参数 source 在 lease 期间停止占用；升级开始前写操作必须完成或得到明确的忙返回。OTA 结束重新建立会话并取状态，不能自动重发升级前未确认的参数修改。
6. 配网密码改为只写，删除读取临时明文的返回路径，确认 App 原配网流程不依赖回读密码；提交后、失败退出及断连清理临时密码。

## 5. 每个文件的落地改动摘要

| 计划文件 | 落地职责 |
|---|---|
| F 本文 | 固化证据、方案、分阶段范围与验收 |
| F ble_ct.c/.h | 精简 INFO、能力协商、session、详细信息投影与 MTU 状态 |
| F ble_ct_cmd.c/.h | 分页读配置、操作查询、状态证据、幂等和有界生命周期 |
| F ble_ct_telemetry.c/.h | 独立快照、读写一致性、订阅节流和分帧发送 |
| F ble_ct_frames.c/.h、组件 CMakeLists.txt | 共用有界消息分帧/重组及编译注册 |
| F ble_prov.c | 配网密码只写、临时值清理、连接生命周期协作 |
| F app_main.c | 统一业务注入、主任务命令推进与操作超时处理 |
| F telemetry.c | 真实字段/采样时标、独立 BLE 刷新周期、DSP/BMS 分组 |
| F cmd_handler.c | 名称到 schema 映射、工程单位、typed 错误与操作结果 |
| F direct_poll.c、direct_link.h | 持久期望值与真实观测分离、revision 与来源、实际确认事件 |
| F direct_ui.c | 与 schema 共用规则，屏幕行为继续按现有交互工作 |
| F direct_param_schema.c/.h、主 CMakeLists.txt | 参数能力、约束、转换和编译注册 |
| F 四个新增 BLE 测试、controller_command_test.py、host runner | 实际编码/传输/快照/操作链测试和入口 |
| A ble_adapter.dart、ble_device_manager.dart | 实际 MTU、长写/分帧、信息握手、操作查询和新旧能力分支 |
| A ble_direct_service.dart、ble_polling_service.dart | 连接、记忆、前后台节奏、首包读取及事件生产 |
| A device_live_snapshot.dart、device_live_data_service.dart | 无 UTC 快照、质量、年龄、代次和跨来源选择 |
| A service_locator.dart、realtime_data_service.dart | 注册汇聚与配置访问，将云与 BLE 数据接到页面 |
| A device_config_access.dart、ble/cloud config source、config_schema.dart | 统一配置接口、离线 schema 与分通道操作实现 |
| A device_realtime_page.dart | 来源、组级年龄、零/未知/过期显示 |
| A device_settings_page.dart、category/advanced 页 | 复用控件、能力过滤、草稿、逐项结果与冲突处理 |
| A device_control_page.dart | 统一操作入口，暂不使用错误的开关机别名 |
| A add_device_page.dart、offline_mode_settings_page.dart | 实际连接开关、状态、SN 核对、取消和重试 |
| A offline_op_log_store.dart | 意图与最终结果关联、兼容存量记录 |
| A app_zh.dart、app_en.dart | 来源、确认级别、过期和操作错误文案 |
| A 五个既有 BLE 服务测试与重组测试 | 连接、传输、生产接线、质量和代次回归 |
| A direct_config_access_test.dart、ble_device_pages_test.dart | 离线配置端到端调用、页面确实使用 BLE、草稿及结果展示 |

## 6. 测试与验收

### 6.1 测试必须覆盖的行为

- INFO 实际 snprintf/cJSON 路径：SN 16 位、当前版本、最长允许版本、最大能力集合、字段缺失、NUL 边界；编码长度满足宣告上限，App 读到真实 session。
- MTU=23/185/247/256/512：每个通知含帧头仍不超过 MTU-3，接收完整消息一致；丢帧、重复、乱序、超时、Read Blob、并发更新、通知队列满均得到明确处理。
- 首次连接立即有已采集快照；没有采样则明确无数据；无订阅时更新缓存；正常读取不能使旧数据变新。
- 负电流 -0.1/-0.5/-1.5A，负温度、零值、最大数字、未知、过期、RTC 未校时、校时跳变、DSP/BMS 各自离线。
- 参数矩阵逐项核对，collector/DSP 枚举转换、步进拒绝、相互依赖阈值、旧 schema、修订冲突以及不支持字段错误；不能只验证代码表中有 key。
- 执行真实 `BLE COMMAND → cmd_handler → queue_user_write → NVS → UART 提交 → 实际观测` 链，单独验证仅保存/仅发送时绝无 applied；无 DSP 确认能力时允许完成并释放写队列。
- 重复 ID 不重复执行，不同内容同 ID 拒绝；回执丢失、App 重启、设备重启、晚到回调、断连查询、重连新 session 不串操作。超时不换云端重发。
- 页面测试在不提供 HTTP 响应时仍能走 BLE 查看/设置，确认没有意外 Dio 请求；多 SN 不串值，草稿不被刷新覆盖，设置开关触发真实 connect/disconnect。
- OTA 独占、配网扫描互斥、远程 MQTT 同时重连、后台切换，验证现有手机 OTA 和云控制不回归。

### 6.2 执行顺序与命令

下面是实施阶段命令，不表示本轮执行或通过。新增测试文件必须先完成，App 目录写入及构建需要所在工作区的文件权限。

第一步：定向固件测试，在 F 目录运行。

```powershell
$blePlanPython = 'C:\Users\29538\.espressif\python_env\idf5.5_py3.13_env\Scripts\python.exe'
& $blePlanPython tests/ble_info_contract_test.py
& $blePlanPython tests/ble_frames_test.py
& $blePlanPython tests/ble_live_snapshot_test.py
& $blePlanPython tests/ble_control_flow_test.py
& $blePlanPython tests/controller_command_test.py
```

第二步：在 A 目录执行现有服务测试和新增页面/配置测试。

```powershell
$blePlanFlutter = 'C:\Users\29538\develop\flutter\bin\flutter.bat'
& $blePlanFlutter test test/ble test/ble_frame_reassembler_test.dart test/features/device/ble_device_pages_test.dart --no-pub
& $blePlanFlutter analyze --no-fatal-infos
```

第三步：固件全量 host 回归与 Device 构建，在 F 目录运行。

```powershell
./tests/run_production_host_tests.ps1
./build.ps1 -Profile Device
```

第四步：在 A 目录构建联调 APK。

```powershell
& $blePlanFlutter build apk --release --no-pub
```

每个命令均检查退出码。实施时核对本机实际工具路径和当前配置，不因环境错误更改产品行为。App 仓库要求的其他受影响构建/检查遵循其 AGENTS.md；本方案未修改后端，不虚报后端检查通过。

### 6.3 实机验收矩阵与建议指标

以下为拟定验收目标，需要记录设备、DSP/BMS 版本、手机型号与系统、固件/APK 哈希后进行。

| 场景 | 应达到的结果 |
|---|---|
| Android/iOS，手机外网关闭 | 可发现、核对 SN、连接、查看已支持数据与设置；iOS 未测则只报告 Android 覆盖 |
| 首次连接及重连各 20 次 | SN 始终正确、无重复会话/永久忙；连接后首次有效快照目标 3 秒内 |
| 前台观察 10 分钟 | 正常采样时显示更新目标 1–2 秒，记录 p95；数据不变可不重绘 |
| DSP 停止回复，BLE 保持连接 | 已有值保留、及时标过期，DSP 异常与 BLE 连接分别显示 |
| DSP 离线、BMS 正常及反向场景 | 正常组继续更新，离线组不伪造为零 |
| 每个已开放参数 | 对照设备观测与持久记录验证结果等级；ESP/DSP 分别重启后检查保存与采用 |
| 正在修改时断开/杀 App/设备重启 | 不自动重放，原操作可查或明确未知；重新连接后仍可继续新操作 |
| MQTT 修改与手机草稿并发 | 发现 revision 冲突，用户草稿保留，不静默覆盖 |
| 配网、参数设置及 OTA 交叉 | 忙状态可解释，没有错误的已生效，OTA 后重新取信息和快照 |
| 同时运行 MQTT TLS、LCD、BLE 24 小时 | 无内存持续下降、看门狗复位；记录 heap 最低值/最大块、任务栈和 DSP/BMS 接收间隔 |

内存预算以当前完整功能样机的空闲堆与最大块为基线。保持 TLS IN=16384 的既有 OTA 接收要求；不能用削减 TLS 能力或永久释放蓝牙内存换取测试通过。具体额外 RAM 上限经实际测量后冻结。

## 7. 分阶段实施和完成条件

| 阶段 | 工作包 | 独立完成条件 |
|---|---|---|
| P0：可连接、协议可信 | INFO 长度、MTU/分帧、能力协商、错误开关机别名和确认语义 | 当前 full-SN/版本握手成功；小 MTU 不丢完整消息；仅串口提交不报 DSP 已应用 |
| P1：手机完整查看 | 实时快照、真实详细信息、连接记忆、数据服务和页面接线 | 无外网可看信息与实时核心数据；显示来源、组级离线、零/未知/过期 |
| P2：常用设置可用 | 参数矩阵/schema、配置 source、操作查询、常用参数与记录 | 常用项按真实能力读写，重启可恢复，断连结果可解释，草稿/并发无误写 |
| P3：完整能力与验收 | BMS 详情、确证后的高级参数、OTA 交叉与 24h 实机 | 参数支持表完整；所有开放能力均有设备证据，未支持项明确只读/不可用 |

按工程经验粗估：P0 1–2 人日，P1 2–3 人日，P2 3–5 人日，P3 2–4 人日另加 24h 稳定运行观察。整体约 8–14 个工程人日，仅作排期参考；DSP 真实确认协议若需修改对端固件，单独评估，不包含在该估算中。

## 8. 待定项与决策边界

- 已确认的本地连接方式：保留连接即用，优先补齐功能；不增加 PIN、屏幕确认或云端授权前置。该产品选择不表示已启动代码实施、刷机或部署。
- DSP 确认：采集当前设备实际 0x25/应答能力；无法证明的项始终显示保存/发送级别。无需等待所有 DSP 协议补齐才交付只读及明确保存语义的功能。
- 常用参数：以上八项是候选；取现有 schema、真实板卡和电池配置交集后冻结首批可写集合。
- 兼容：拟新增 capability、消息上限、分页格式及结果证据字段在 P0 冻结；旧 App、旧 L10 与 direct 固件分别覆盖。旧文档的强鉴权/参数范例不能直接当成现状协议。
- 物理开关机：等待真实命令与状态证据，不能沿用 output_priority 别名冒充。
- 工作区：固件文档当前可写，App 源码已完成只读核查。未来实际改 App 时需要允许主 App 仓库的写入和构建输出；不能在固件目录复制一个 App 当作交付。

建议下一步仅启动 P0，先交付可复现的 INFO 和传输修复，以及准确表达保存/发送/确认的协议与测试结果，再进入手机页面接线。
