# 扫码配网与云端绑定 Implementation Plan

> **For agentic workers:** REQUIRED: Use subagent-driven-development or executing-plans skills to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking. 已实施固件、App、服务器和工位工具的软件部分；逐项实机验收仍未完成。当前状态和交付说明见 ../../research/2026-10-13_扫码引导实施与验收.md，不能将计划中的实机步骤视为已验证。

**Goal:** 在 CS-L10-6K2 屏幕显示可扫描的 SN/PIN 与操作引导，并串联手机配网、云端绑定，全程保留正常本地使用。

**Architecture:** 独立引导状态机消费联网、身份和云绑定快照；生产 PIN 与 SN 匹配存储；云端作为绑定状态唯一来源。复用现有分条绘制、BLE 配网、MQTT 和 App 绑定页，通过版本化接口逐步上线。

**Tech Stack:** ESP-IDF 5.5.4/C、NVS、Nayuki qrcodegen C、现有 Monocypher、Flutter/Dart、Go/PostgreSQL/MQTT、Python/CustomTkinter/PyInstaller。

---

## 执行边界与基线

- 固件根目录 `D:/CS_INV_WIFI/esp32_arm_replacement`，继续独立项目；当前软件发布基线 v0.6.3，实机状态需在实施前确认。
- App/云端根目录 `D:/CS_APP_PROJECT/cs_inv_monitor-develop/cs_inv_monitor-develop`。当前工作区权限只允许读取此仓库；实施相关修改时需取得实际写入权限，不在固件目录创建冒充主仓库的副本。
- 下文 `F/` 和 `A/` 分别代表上述两个绝对根目录；文件路径明确区分“现有”与“新增”。
- 本次不烧录、不部署、不提交 Git。实施时先记录已有改动，仅按任务白名单整理补丁；提交和部署按当时用户授权进行。
- 首发只支持辰烁 App 扫码；正式 HTTPS 下载落地页作为后续兼容项，不让它阻塞设备内的基本流程。
- 复用现有字体/主题/图形库，不增加 LVGL、整屏 framebuffer、新硬件或 GPIO。

## 冻结的交互与协议决定

1. 自动引导在品牌动画后、第一次 UI 帧前判定，未操作 20 秒返回首页；手动打开的扫码页在 60 秒无操作后返回原页面。扫码页存续期间不被原息屏计时提前关闭，退出即恢复原策略。
2. 用户主动选择“稍后设置”按 SN 持久保存，不在重启后反复弹出；自动超时本次启动不再弹出，下次开机允许再次提示。真实绑定/配网状态变化只更新首页提示，不能抢占操作页面。
3. 告警、工厂测试、OTA、待确认参数写入和参数编辑优先；二维码页不能改变 DSP/BMS 轮询、控制输出或升级策略。
4. QR 内容固定 `csinv://bind?sn=<16位SN>&pin=<6位PIN>`，PIN 是铭牌绑定 PIN，与串口管理员密码不同。短 SN 保留原功能，但该设备显示“不支持扫码，请检查设备识别码”；不补零、不偷偷修改 SN。
5. 只有收到经过可信来源校验的云端状态才设置 UNBOUND/BOUND；断网保留缓存。BLE INFO 的 `bound=true` 保持旧兼容含义。
6. PIN 缺失或与 SN 不符时显示“设备识别码未配置”，不要生成随机 PIN/假二维码；不限制逆变器工作。
7. 量产 SN 统一为 16 位大写字母数字。现有 App 会将 SN 转大写，而 PIN 算法按原字节计算：对已有小写 SN 不偷偷改大小写，先走身份维护；否则扫码身份可能与实际 MQTT 身份/PIN 不一致。

## Chunk 1：身份与生产 PIN（可单独交付）

### Task 1：SN/PIN 存储契约和掉电行为

**现有文件：** `F/main/factory/factory_identity.[ch]`、`F/main/factory/factory_service.c`、`F/main/app/app_main.c`、`F/main/CMakeLists.txt`。

**新增文件：** `F/main/factory/factory_pairing_record.[ch]`、`F/tests/factory_pairing_record_test.c`、`F/tests/factory_pairing_identity_test.c`。

- [ ] 定义单独的 version=1 配对记录：MAC、原样 SN、六位 PIN、递增 generation、CRC32；显式字节编码，不能直接持久化 C struct。
- [ ] 先写测试：前导零、SN/PIN 格式、错误 MAC、CRC 损坏、同 generation 冲突、计数耗尽。
- [ ] 在 factory 分区 identity 命名空间增加 `pair0`/`pair1` 两槽，按已验证 generation 选择；保留原 336 字节身份格式和分区表。
- [ ] 暴露只读 `factory_identity_pairing(sn, out_pin)`：必须同时匹配当前 SN 和硬件 MAC；没有记录/旧 SN/损坏都返回不可用。
- [ ] 暴露工厂写入 `provision_pairing`：工厂写权限、OTA 排他、sticky lock、写后读回必须全部满足；v0.6.3 的现有命令编号保持兼容，新命令号先核对完整分发表后登记到协议文档。
- [ ] 覆盖“SN 已写、PIN 未写即断电”和“PIN 写入另一槽中断”：重启后要么得到匹配的完整记录，要么不可扫码，不能用旧 PIN；工具不能把部分成功报告为完成。
- [ ] 验证 `set_sn` 后读 PIN 因 SN 不匹配立即失效；同 SN 重写和锁定后的幂等检查行为明确。旧 v0.6.3 回退能忽略新侧槽，不损坏原身份。

**验证命令：** 将两个实际代码测试加入 `F/tests/run_production_host_tests.ps1`，在 F 执行 `./tests/run_production_host_tests.ps1`。预期 PASS 包含坏槽回退、掉电不显示旧 PIN、旧身份/锁定回归。

**完成条件：** 不改变既有 SN 管理权限；旧固件不需要分区迁移；任何失败不显示错误配对凭据。

### Task 2：生产工具配对写入和已有设备补写

**现有文件：** `F/tools/factory_tool.py`、`factory_gui_backend.py`、`factory_gui_modern.py`、`build_factory_gui.ps1`、`package_factory_gui.py`。

**新增文件：** `F/tools/factory_pairing.py`、`F/tests/factory_pairing_tool_test.py`。

- [ ] 用虚构测试密钥写出与后端完全相同的 HMAC 前三字节算法测试；同一组测试向量由固件侧记录测试与后端 PIN 测试复用。
- [ ] 工位从运行时环境读取 PRODUCT_SECRET；不编入 EXE、固件或 App，不放入日志和发布包。缺少配置时明确失败。
- [ ] GUI 身份页增加“写入匹配 PIN/校验扫码身份”，串行执行 SN 写入、PIN 写入、读回一致性检查；不自动锁定，仍沿用原生产审核锁定操作。
- [ ] 设备 capabilities 新增 `pairing_identity_v1`，旧固件不支持时显示需更新；避免工具根据固件版本字符串猜能力。
- [ ] 在设备实际命令分发表核实编号后，新增能力查询、PIN写入/校验命令；文本补写语法拟定 `set_pin <管理员密码> <当前SN> <六位PIN>`，复用已有密码摘要校验、输入擦除和禁止回显。只接受当前运行SN，设备重新上电后再补写新SN的PIN。
- [ ] 已有 Device 配置提供管理员认证的 PIN 补写流程：复用 USB 文本/二进制拥有者和 OTA 排他；旧 set_sn 语法不变。为已有未锁定设备补写，不伪造完整生产档案。
- [ ] 普通巡检/导出默认只显示 PIN 是否就绪；SN/PIN 铭牌导出作为显式操作；GUI 不显示产品密钥。准备当前开发机的专属补写步骤，不能把共用默认 SN 用于批量设备。
- [ ] 编译新 EXE，执行冻结包自检、旧固件兼容测试和发布包秘密扫描；软件成功与真实读回证据分别记录。

**验证：** 在 F 执行 `python tests/factory_pairing_tool_test.py`，预期 PIN=000xxx 原样保留，部分写入拒绝完成，旧固件能力缺失不发送新写命令。随后运行实际 GUI 冻结自检入口（从当前构建脚本读取，不猜命令）。

**完成条件：** 可以给当前已有设备补写 PIN，也可以量产写入；不把产品密钥随 EXE 分发。

## Chunk 2：可信的云端绑定状态（固件可先做离线测试）

### Task 3：确定服务器状态来源与所有写路径

**现有文件：** `A/business-api/internal/service/services.go`、`A/business-api/internal/repository/repositories.go`、`A/business-api/internal/handler/device_binding_handler.go`、`A/business-api/internal/handler/device_claim_transfer_handler.go`、`A/device-communication/internal/repository/device_repository.go`、`A/device-communication/internal/mqtt/client.go`。

**新增文件：** `A/device-communication/internal/service/device_binding_state.go`、同目录 `_test.go`、`A/business-api/internal/repository/binding_state_revision.go`、同目录 `_test.go`；迁移文件加入现有 `business-api/internal/migration` 注册流程。

- [ ] 列出 Bind、Unbind、审核解绑、认领、批准转移、管理员批量导入/所有权变更的实际数据库写路径；明确“有有效所有者”才是 BOUND。
- [ ] 所有权变更事务内递增 binding_revision；重复的幂等绑定不产生虚假变更。不存在 SN 返回 UNKNOWN，不当成未绑定。
- [ ] 先写 PostgreSQL 集成测试验证事务回滚、并发变更和所有写路径 revision；不得只测新辅助函数。
- [ ] 定义上行 `cs_inv/{sn}/binding/query` 与下行 `cs_inv/{sn}/binding/state`：查询带每次生成的 nonce，回应来自数据库当前快照，包含 sn、state、十进制字符串 revision、原 nonce。
- [ ] 核对 MQTT 账户与发布权限。当前设备配置使用共享账户，不能凭“在自己 topic 收到消息”就认定是服务器消息；采用服务器签名状态，设备只持公钥，签名域与 OTA 分离。
- [ ] 签名规范固定为 ASCII：`CSERGY-BINDING-V1\n<sn>\n<state>\n<revision>\n<nonce>\n`，Ed25519 签名转 hex；正文无 PIN、账号名和其他个人信息。公钥 key_id=1，正式私钥只在受控服务器配置，测试使用虚构密钥。
- [ ] `binding/state` 使用 QoS1、非 retained 查询回应。变更通知只提示设备重新查询；首次/重连/变更后查询；同一在线会话每 5 分钟兜底重查，离线不忙等。
- [ ] 明确是否有可靠现有事件通道可复用：有则事务提交后通知；没有则首版依赖 5 分钟重查，不新增不必要的消息平台，并在 App 上明确“设备状态稍后同步”。

**验证命令：** A/business-api 执行 `go test ./internal/service ./internal/repository ./internal/handler ./internal/migration`；A/device-communication 执行 `go test ./internal/service ./internal/repository ./internal/mqtt`。数据库集成测试必须配置专用测试数据库且实际运行，未运行记为未验证。

### Task 4：固件解析、缓存与查询

**现有文件：** `F/main/mqtt/mqtt_handler.c`、`F/main/app/app_main.c`、`F/main/CMakeLists.txt`。

**新增文件：** `F/main/onboarding/cloud_binding.[ch]`、`cloud_binding_codec.[ch]`、`F/tests/cloud_binding_test.c`。

- [ ] 先写真实 codec 测试：错误 SN、nonce、签名、公钥、超长消息、旧 revision、重复消息、计数上界；按签名规范验证交叉语言向量。
- [ ] 独立状态 UNKNOWN/UNBOUND/BOUND；只接受当前 outstanding nonce 的有效签名回应，旧查询回应和重放不更新状态。
- [ ] 将验签和 NVS 写入移到普通工作任务，MQTT 回调只投递有界消息；不阻塞客户端接收或 UART。
- [ ] 按 SN 缓存最后确认状态与 revision；重复状态不重复写 Flash；断网保持缓存，启动 UNKNOWN 不能误显示未绑定。
- [ ] 订阅就绪后查询，10 秒响应超时、30/60/300 秒退避；主动查询与变更通知共用限流，不生成并行查询。
- [ ] SN 变更隔离旧缓存；删除 Wi-Fi 配置保留匹配身份的绑定缓存；本地界面恢复默认不清除云所有权。整片擦除后回到 UNKNOWN。
- [ ] 老服务器不回应时只提供配网引导，不能猜 UNBOUND；故障仅限云连接提示。

**验证：** 加入主宿主回归脚本；预期 PASS 包含实际签名校验、状态转换、缓存掉电、NVS 写次数、消息队列边界和没有周期串口刷屏。

**完成条件：** 不修改 BLE INFO `bound` 的既有含义；绑定状态对任何逆变器控制动作都没有使能/禁止权限。

## Chunk 3：屏幕引导和二维码（可用模拟数据独立验收）

### Task 5：独立引导状态机

**新增文件：** `F/main/onboarding/onboarding_state.[ch]`、`F/tests/onboarding_state_test.c`。

**现有文件：** `F/main/direct/direct_ui.[ch]`、`F/main/direct/direct_panel_runtime.inc`、`F/main/storage/nvs_storage.[ch]`。

- [ ] 用纯 C 状态测试先覆盖品牌动画结束、首次未配网、明确未绑定、UNKNOWN、断网、人工退出、自动超时、重启、SN 变更。
- [ ] 新增 UI_PHONE_SETUP 与受限显示事件；输入快照含 has_wifi、wifi_connected、binding_state、pin_ready、sn、抑制条件。
- [ ] 判定只在首次 UI 帧前自动执行一次；返回键不触发首页其他操作，确认键只显示步骤；没有新的全局按键组合。
- [ ] 主动退出才持久化 dismissed_sn；自动超时不写 Flash。本次启动自动引导最多一次，告警延期结束后不在编辑/菜单中抢占。
- [ ] 一次性保存原页面和息屏上下文；退出/超时/报警/升级后恢复有效上下文，不能回到过期编辑草稿。
- [ ] 首页只加可发现的蓝青提示；设置增加“手机连接”，成功后显示连接状态而非重复绑定提示。

**验证：** 扩展 `F/tests/ui_v050_test.c` 和新增状态测试，执行主回归脚本。预期 20s/60s 边界、计时溢出、alarm/OTA/factory/编辑优先级通过。

### Task 6：QR 编码、像素布局与美术

**新增文件：** `F/main/onboarding/onboarding_qr.[ch]`、`F/main/direct/direct_onboarding_scene.[ch]`、`F/tests/onboarding_qr_test.c`、`F/tests/onboarding_render_test.c`；第三方库存放 `F/components/qrcodegen` 并保留 MIT 许可。

**现有文件：** `F/main/direct/direct_ui_scene.c`、`direct_ui_locale.c`、`direct_ui_scene.h`、`F/tests/ui_render_test.c`、`F/main/CMakeLists.txt`。

- [ ] 固定短链接格式、ECC M、允许版本 1～6；静态缓冲仅按上限分配，发布前记录实际静态 RAM/栈预算；编码失败进入可手输页面。
- [ ] 单独统计 QR两个矩阵缓冲、payload、快照和状态机的常驻预算，目标合计不超过4KB；验签工作栈另计并实测高水位。超限需记录原因并调整，不能挤占OTA已有必要余量。
- [ ] 与独立解码器验证最大 SN、000000 PIN、大小写规则和确切 payload；不能只比较自身编码器输出。
- [ ] 左卡片 156×156：模块整数倍率 ≥3px、四模块留白；码点严格黑白、无滤波/中心 logo；超限不得挤压缩放。
- [ ] 顶部 26px，左右内容从 y=34 开始，SN 区 y=198，操作栏 y=220；文字在 320×240 实际像素内布局，插图在二维码卡片以外。
- [ ] 中英、深浅四种组合，点阵字高统一，SN 16 字符完整、PIN 六位保留零。美術用蓝/青/绿，不用灰色图标或低对比文字。
- [ ] QR 只在 payload 改变/首次进入时生成；外围静态手机图标首版即完成，成功状态轻动效后续加；不要为动效周期重绘整个二维码。
- [ ] 用现有 RGB565 320×16 分条缓冲渲染，验证全帧/分条像素相等、越界哨兵、字形覆盖与差分区域。
- [ ] 更新品牌启动顺序测试：先清屏和 logo，再扫码页或首页，不出现首页→扫码页→首页的闪跳。

**验证：** 主回归脚本 PASS；生成四种 1:1 截图与放大预览到 `F/docs/ui_redesign/qr_onboarding`；图形验收后再接真实状态。

## Chunk 4：手机扫码→配网→绑定（必须在 App 主仓库实施）

### Task 7：扫码解析契约统一

**现有文件：** `A/inv_app/lib/core/utils/sn_utils.dart`、`core/services/deep_link_service.dart`、`core/utils/qr_scan_guard.dart`。

**新增/扩展测试：** `A/inv_app/test/core/utils/onboarding_qr_test.dart`、现有 `test/core/services/deep_link_service_test.dart`、`test/core/utils/qr_scan_guard_test.dart`。

- [ ] 先写测试同时覆盖 csinv://bind、现有 HTTPS /bind、铭牌 SN/PIN、单 SN，保持旧二维码兼容。
- [ ] 明确 csinv 的 bind 是 URI host、HTTPS 的 /bind 是 URI path；当前扫码解析器没有 csinv 分支，必须修复。
- [ ] 统一 SN/PIN 校验，SN16字母数字、PIN六位数字；拒绝重复冲突参数、错误格式；扫码入口与冷/热启动 deep link 输出相同对象。
- [ ] 同一码只能启动一个绑定流程；返回/取消后允许用户重新扫描；PIN 不写普通日志或分析事件。

**验证命令：** 在 A/inv_app 执行 `flutter test test/core/utils/onboarding_qr_test.dart test/core/services/deep_link_service_test.dart test/core/utils/qr_scan_guard_test.dart`。预期正确短链接实际进入设备扫码绑定路由。

### Task 8：BLE 配网和云绑定状态流

**现有文件：** `A/inv_app/lib/features/device/presentation/pages/device_qr_bind_page.dart`、`wifi_config_page.dart`、`core/services/ble/ble_binding_service.dart`、`core/services/ble_provisioning_service.dart`、`core/router/app_router.dart`、`features/device/data/repositories/device_repository_impl.dart`、`lib/l10n/app_zh.dart`、`app_en.dart`。

**新增文件：** `A/inv_app/lib/features/device/presentation/services/qr_onboarding_coordinator.dart`、`A/inv_app/test/features/device/qr_onboarding_test.dart`。

- [ ] 先写 mock 流程测试：扫码确认→未登录→登录恢复→蓝牙发现→按读取 INFO 的完整 SN 匹配→用户选 Wi-Fi→connected 通知→云端绑定成功。
- [ ] 不能仅凭 BLE 广播名称做匹配；INFO SN 不符立即停止，不向错误设备写 Wi-Fi。
- [ ] 已联网设备允许直接云端绑定；云端离线/连接状态失败提供明确“通过蓝牙配网”入口，不自动抹掉原 Wi-Fi。
- [ ] 保留扫码 PIN 到配网完成后的云端请求；不把 BLE bound=true 当成账号绑定成功，也不新增本地 PIN 鉴权门槛。
- [ ] 手机权限拒绝、蓝牙关闭、Wi-Fi 密码错误、无 2.4GHz AP、App 退后台、绑定属于别人、超时重试均有取消路径；取消不关闭逆变器、不撤销已有运行设置。
- [ ] UI 显示“连接设备→联网→添加到我的设备”三个真实阶段，只有服务器绑定成功才显示完成；后台同步未完成与绑定业务失败分开。
- [ ] 设备不连互联网而用户只想本地蓝牙操作时保留原功能；联网后可再绑定，不出现强制登录锁设备。

**验证命令：** `flutter test test/features/device/qr_onboarding_test.dart`；对上述修改文件执行定向 `flutter analyze`。预期所有 mock 阶段通过，原 BLE OTA/配网/登录跳转回归通过。

## Chunk 5：联调、资源测量与正式交付

### Task 9：先验证，再分阶段发布

- [ ] 给当前设备预写匹配 SN/PIN，实测旧设备升级无需整片擦除，factory/NVS 数据不变。
- [ ] 从品牌动画开始录像：未配网扫码页、返回立即使用、操作 20s/60s、报警打断、重启不弹；记录真实按键响应及完整设备日志。
- [ ] Android 和 iPhone 分别完成四种主题/语言扫码，正常背光下正面与合理斜角验证；码内容应与设备当前 SN/PIN 完全相同。
- [ ] 真机跑未配网、联网未绑定、已有所有者、无互联网、错误 PIN、SN修改、解绑、转移、服务器旧版本、MQTT重复/延迟/断线的端到端矩阵。
- [ ] 对比 v0.6.3 与新候选在相同状态下的最小堆/最大空闲块、面板渲染耗时、DSP04接收间隔、BMS完整帧、USB命令与 BLE/MQTT OTA；不能用“二维码能扫”代替基本功能回归。
- [ ] 实施前采集性能基线后写入验收表：按键响应不得明显退化、无看门狗/欠压新回归、不持续刷日志、不增加整屏缓冲。TLS升级所需最大空闲块不足时先减预算，不掩盖错误。
- [ ] 至少连续运行 24 小时，覆盖连接/断开、进入/退出、服务器状态更新与掉电恢复；保留测试固件哈希和设备SN，报告中遮盖 PIN。
- [ ] 构建 Device/Factory/Production 三配置，执行 `./tests/run_production_host_tests.ps1`、`./build.ps1 -Profile Device|Factory|Production`（三次分别串行），每次构建后 `python tools/package_production.py <profile>`；切勿并行共用 build 目录。
- [ ] 新版本号在开始实施时从当前 version.txt 递增；不修改 v0.6.3 既有发布文件。GUI EXE另递增版本，冻结自检/文件哈希/秘密扫描齐全。
- [ ] 服务器先兼容上线，随后生产工具、App、固件；旧客户端和旧设备继续工作。部署和实机烧录需具体执行授权，研究计划本身不代表已授权发布到生产服务器。
- [ ] 发布包包含保留设置的更新方式、4MB覆盖提示、SN/PIN补写指南、测试矩阵、哈希、服务器与App最低兼容条件。

### 回退策略

- QR 功能关闭后首页与原菜单仍可用；不修改 DSP/BMS 协议及旧控制参数语义。
- 原身份 schema1/分区不变，v0.6.3 忽略配对侧槽；新绑定缓存使用独立 NVS namespace。
- 新接口缺失：UNKNOWN＋可手动配网；签名失败：保留上次可信状态；PIN无效：手动引导；都不限制本地使用。
- 老 App 不能扫描 csinv 码时保留手输 SN/PIN，并明确升级 App；不可宣称所有旧 App 兼容。
- 服务器签名公钥未配置/生产工具无真实 PIN来源/实机扫码未验收，均不作为完整功能发布。

## 阶段交付与依赖

| 阶段 | 可审阅成果 | 依赖 |
| --- | --- | --- |
| 1 身份 | PIN记录测试、补写工具和EXE | 真实工位密钥配置，仅运行时读取 |
| 2 状态 | 签名协议向量、云端测试、固件缓存 | App/云端写入权限、正式服务器签名配置 |
| 3 视觉 | 四种截图、状态机/像素测试、固件候选 | 阶段1与2接口；可先用测试快照 |
| 4 App | 扫码/蓝牙/配网/绑定流程和APK | App写入权限、可测试后端 |
| 5 验收发布 | 真机矩阵、24h记录、release包 | 用户设备与两种手机，生产部署授权 |

首个实施批次建议做阶段1和阶段3的纯状态/视觉部分，随后接阶段2和阶段4；没有真实云端确认之前不把测试快照当成上线状态。任务可按测试点拆分提交，不混入现有无关改动。

## 计划审查记录（2026-10-13）

- 已再次核实固件NVS/生产命令入口、Monocypher Ed25519组件、App扫码解析器、BLE配网接口、后端实际Bind/Unbind数据库路径；发现并列入计划的兼容问题是 csinv 扫码缺失、SN大小写与PIN字节不一致、旧 bound 字段不代表账号绑定。
- 补充掉电期间SN/PIN错配拒绝、已锁定设备不能随意补写、共享MQTT凭据下服务器消息可信校验，以及原始身份schema回退策略。
- 独立计划审查代理因账户使用额度限制未能运行；尚无独立审查通过结论。实施前再做独立审查，此记录不阻止用户审阅或继续完善方案。
- 本轮只产生计划文档，未实现新命令、二维码、服务器签名或App流程；上述测试命令均为实施时验证步骤，不能据此宣称已经通过。
