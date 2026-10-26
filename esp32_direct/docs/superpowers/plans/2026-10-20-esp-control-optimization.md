# ESP32 控制链路优化实施

用户授权执行上一轮五项优化，明确排除服务器证书校验。保留既有工作区改动、TLS 参数、分区、版本及证书行为；不刷机、不部署，不开放未确认的 AC 命令，不声明 DSP applied。

Route: develop-feature / Full / multi-session。命令并发与共享状态需要行为验证；使用 superagents、TDD、独立评审和统一构建。

## 设计与验收

1. MQTT 回调只做有界接收，普通 cmd 使用固定容量队列，主循环每次最多消费一条。不在回调解析 JSON/发布回执。retained 普通写命令必须拒绝，查询可以读取。
2. 先查已存在的任务并重放，未存在任务经过期限/类型/参数检查后才占写槽。纯查询独立于写缓存；无效、过期、未执行任务不挤掉已有写记录。有效写的10分钟幂等窗口保留。
3. 有界队列满计数并可诊断，拒绝新输入而不覆盖已排队消息；接收重发、出队过期、并发/回调顺序均有测试。
4. 底层区分 busy、invalid、unsupported、OTA/Boot、not-ready；保存/串口提交失败也给出明确原因，云端与 BLE 兼容现有数值码并扩展字符串结果。
5. DSP 原始帧按内容变化/新异常且有限频率打印，常态保留摘要与计数，USB status 仍可查看证据。owner 不格式化。
6. 资源预算工具读取实际构建产物，报告 app分区余量、静态RAM和主要占用，对预算越界失败。运行时最大连续堆和栈水位仍要实测，禁止盲目缩栈/TLS；用构建前后数据说明收益。

## Session Plan / Ownership

| session | role/runtime | owned paths | dependencies | done signal |
| --- | --- | --- | --- | --- |
| root | implementer | cmd_handler.c/.h、mqtt_handler.c、app_main.c、命令测试、主机runner、本文档 | 既有baseline | cache/queue红绿、合流验证 |
| dsp_optimization | implementer | direct_poll.c、direct_link.h、arm_poll.h、ble_ct_cmd.c/.h、DSP/BLE测试 | root错误映射契约 | 精细返回码、节流红绿 |
| resource_budget | implementer | 新预算工具/测试、main/CMakeLists.txt或root CMakeLists仅必要预算集成 | 构建baseline | 报告与门槛可运行 |
| reviewer | reviewer | none | 三者合流 | 无未解决关键问题 |

Shared read-only: 其他实现者owned paths、sdkconfig*、证书与密钥、现有构建。所有工具输出写tmp/，禁止扫描或输出凭据。

## Tasks

- [x] 保存当前相关文件与构建baseline。
- [x] 失败测试：query/invalid不挤占写槽、MQTT只入队、retained写拒绝、queue满/出队过期/重放。
- [x] 实现并测试缓存准入、固定队列和主循环消费。
- [x] 实现并测试失败原因和日志节流。
- [x] 增加资源预算工具，运行baseline报告和最终报告。
- [x] 独立合流评审、完整生产主机回归、独立ESP-IDF构建。

## Merge Review

核对错误码枚举、云端/BLE映射、主循环顺序、测试抽取依赖、证书行为未改。外部服务/App不在本轮修改范围。失败先修复再统一验证，不以局部测试代替合流。

独立评审未发现 P0/P1 阻断项；建议的不同载荷 FIFO/溢出与错误码映射覆盖已补齐并通过。资源预算由 arm_control_evidence session 实现，root 在 build.ps1 集成构建后检查；DSP session 还完成本地屏幕错误文案与测试。未改 CMake、sdkconfig.defaults、证书、TLS、分区或版本。

## Handoff

本地实现与验证完成。完整生产主机回归和独立 ESP-IDF 构建均 exit 0，资源预算通过，详情见 docs/ESP_CONTROL_OPTIMIZATION_20261020.md。现场性能、最大连续空闲堆、主循环及 UART owner 真实栈裕度、MQTT服务器和物理设备均未验收。版本仍为 0.6.39-mqtt-ble-coexist，未刷机或部署。
