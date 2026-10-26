# 远程控制 P0 实施与验收

**Goal:** 完成已讨论的 task_id 去重、期限透传/拒绝和真实回执阶段；App 按能力显示开关机。
**Architecture:** 保持现有 MQTT 命令路径；固件只声明本地持久化与串口提交，后端以 submitted 独立收口，客户端明确生效未确认。
**Tech Stack:** ESP-IDF C/cJSON、Go、Flutter、React。

用户以粘贴的完善方案和“继续”授权本次实施。保留工作区已有更改；本轮不刷机、不部署、不执行真实设备写命令。DSP 输出开关语义和并机保护仍属 P1。

## Session Topology

| session_id | role/runtime_agent | goal | depends_on | owned_paths | done_signal |
| --- | --- | --- | --- | --- | --- |
| root | planner/implementer | 固件及统一验收 | 用户方案 | main/telemetry/cmd_handler.c、tests/cloud_command_replay_test.py、本计划 | 编译和行为测试 |
| backend_p0 | implementer | 期限与阶段状态机 | 固件契约 | .remote-control-p0/backend 候选；外部 Go 源码只读 | 清单、候选、测试 |
| app_p0 | implementer | 客户端阶段与能力过滤 | 同一契约 | .remote-control-p0/ui 候选；外部 UI 源码只读 | 清单、候选、测试 |
| reviewer | reviewer | 合流检查 | 三部分完成 | 只读 | 无未解决的关键问题 |

## Contract

- 写命令接收：stage=acknowledged, result=OK；这不代表执行成功。
- 保存及全部串口帧提交完成：stage=submitted, result=OK，submitted_args 和 desired_revision 仅为期望配置。
- 本固件没有 DSP 采纳证据，不发送 applied，不用 applied_args/reported_revision 冒充采纳证据。
- 查询请求：stage=completed；失败：stage=failed，保留明确 result。
- 同 task_id、同有效命令内容重发：返回缓存回执，不再执行；不同内容：IDEMPOTENCY_CONFLICT。
- 缓存仅在本次启动期间有效；8 项、10 分钟；容量满且无过期项时拒绝新任务，避免提前驱逐导致重放。处理中项不驱逐。
- expires_at 为 Unix 秒整数。带期限命令遇到不可信时钟则 CLOCK_UNSYNCED；到期则 EXPIRED；保留旧无期限协议兼容。

## Tasks / Verification Gate

- [x] 固件：先以实际 cJSON 分派器编译主机测试，复现重发再次写入；实现并发安全缓存和阶段；测试冲突、过期、重启、容量、串口失败及 JSON 转义。
- [x] 后端：先测期限被丢弃；透传期限；submitted 独立终态，ACK 不误发成功通知；旧协议兼容和单调状态。
- [x] UI：能力未知默认不可开关；submitted 单独文案，ACK 继续等待；测试成功/失败/提交状态。
- [x] 合流：原始副本哈希确认未被并发更改；外部写入经受控复制；逐项检查契约及写入边界。
- [x] 统一验证：固件 ESP-IDF 构建、云端/BLE行为回归；Go build/test/vet；涉及客户端的定向测试及分析。

## Merge Review

所有候选已按 base SHA256 校验后应用，后端 23 文件、UI 10 文件互不重叠。独立 reviewer 已复核串口回调、无堆终态序列化、冲突不覆盖原任务、单调 SQL、旧 log-only 和 direct INFO 能力证据。未发现未解决的 P0/P1；普通通知仍为原有 best-effort。

## Handoff

本轮实现和本地验证完成。固件编译、5 项行为回归、App 7 项测试及 analyze、后台 tsc/ESLint/2 项测试已通过；实际两后端全模块 build/test/vet 和 3 项 PostgreSQL 集成均通过。未部署、未刷机、未实机验收。结果与边界见 ../../REMOTE_CONTROL_P0_20261020.md。
