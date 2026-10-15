# L10 Serial Admin SN Implementation Plan

**Goal:** USB 串口输入 `set_sn <password> <sn>`，写 ARM 后回读验证，再更新 ESP NVS 和重启。

**Architecture:** 主循环非阻塞读取 USB；独立纯 C 行解析器不回显密码。ARM 调度器持有单个 SN 事务，CS 0x09/0/16 写入，0x09/0/1 状态应答后 CS 0x06/0x0190/52 回读。OTA 服务通过现有锁保留配置操作，阻止双向争用。

**Tech Stack:** ESP-IDF 5.5.4 / ESP32-C3 / FreeRTOS / CS TEA+CRC32+PPP / native C host tests。

## Chunk 1: implementation and verification

- [x] 写 `tests/host/test_admin_sn_command.c`：正确/错误认证、SN 长度字符、CRLF、退格、控制字符、超长整行丢弃、密码缓冲擦除。先运行缺失功能验证，再添加 `main/app/admin_sn_command.c/.h`。
- [x] 扩展 `tests/host/test_arm_bms_poll.c`：真实 CS 编解码和调度，写请求字节、错误/无关应答、超时、旧 0x06、成功回读、NVS 失败、忙时拒绝与回归。先运行失败，再修改 `arm_poll.c/.h` 和 `cs_protocol.h`。
- [x] 添加 `main/app/serial_admin.c/.h`，接入 `app_main.c` 主循环及 `main/CMakeLists.txt`。管理员密码独立 Kconfig 配置，按用户指定值设置默认。
- [x] OTA `ota_service.c/.h` 添加配置操作预留接口；与 manifest 预留共用锁，释放与错误路径配对，禁止 SN 操作时启动 OTA。
- [x] 增补对接文档：明文 CS 字段、小端、零填充、状态码、ARM 持久化时序、错误后的恢复与验收。ARM 源码不在本仓库，提供独立接入示例，不宣称 ARM 已实现。
- [x] 从既有 `build/host-tests-gcc` 读取工具路径配置宿主测试；执行构建与 CTest。使用既有 ESP-IDF 环境增量构建，保留预存 sdkconfig/用户更改。
- [x] 依据 verify 技能检查完整成功/失败行为，再做独立审查。无需新增用户确认或提交/烧录。

## Acceptance

密码错、非法输入、OTA 忙不发帧；写错或读回不一致不保存 ESP SN；旧只读应答不触发意外重启；确认成功仅保存一次，旧身份 retained 清理在 NVS 成功后；NVS 失败报告 ARM 已写但 ESP 未保存，允许重新下发恢复。后续硬件验收需 ARM 集成 0x09 后进行。
