# 0.6.37 MQTT 连接内存与重连修复

## 现场证据与边界

- SN H1ZZX00139000101，现场版本 0.6.36-memory-fix。
- MQTT 在约 1.9 秒、DHCP 完成前发起 DNS，立即失败；第一次有效联网重试约 11.9 秒，和 10 秒 BLE 启动等待期重合。
- 连接重试时内部空闲 RAM 约 11 KB，最大连续块 7680 字节；另一轮记录最低空闲仅 476 字节。记录的是 internal / 8-bit heap，不是任务剩余栈。
- DSP 在约 4.2 秒开始收到 03/04，在 34/64 秒持续收到新数据，CRC、长度与 UART 错误为零。本次不修改 DSP 查询、解析、参数同步与 IAP 路径。
- 欠压由用户手动断电造成，不作为本次修复根因。
- 服务器 2026-10-19 15:17（北京时间）探测：公网 IP 101.43.90.25，TCP 8883、TLS 1.2、MQTT CONNACK 均有响应。匿名拒绝认证只证明 broker 正在处理请求，不说明设备凭据错误，也不证明设备网络可达。

## 修复行为

1. 在网络启动前预留 8192 字节 MQTT 栈加 1024 字节分配余量；真正创建 MQTT 任务前释放预留。无 IP、无准入时不启动 DNS/TCP/TLS，不消耗失败重试计时。
2. 关闭 SDK 自动重连，由主循环统一准入，失败后退避 5 秒。已运行任务调用 reconnect，不重复创建任务。
3. 等待启动健康检查结束、启动栈回收后，再允许网络连接。健康检查不以云端在线为前提。
4. MQTT 初次连接及后续重连使用独占内存窗口。若 BLE 已启用且空闲，完整暂停 host/controller 并释放其动态内存，不仅停止广播。TLS 尝试结束前不恢复 BLE。
5. 每轮云端尝试目标窗口 30 秒；已开始的握手不被窗口硬切断。服务器持续不可达时给出 15 秒 BLE 访问窗口，活跃 App/配网/本地 OTA 优先，结束后再恢复云端重试。
6. MQTT 与 OTA 使用独立的 BLE 暂停所有者；最后一个所有者释放后才能恢复 host。生命周期操作串行执行，任务创建失败完整回滚。
7. SDK transport 创建可能静默退出、不发 DISCONNECTED。为进入 BEFORE_CONNECT 前的阶段设置 2 秒看门限，离线期间也执行清理；进入连接阶段后使用显式 10 秒 SDK 网络超时。
8. 保留 MQTT 8192 字节任务栈及 TLS IN=16384、OUT=2048、动态 TLS 缓冲。未降低 CRC/帧校验，也未把通信缓存当成新数据。

## 验证方法

- `tests/mqtt_connection_window_test.py`：提取真实 C 决策函数，验证 DHCP 前不连接、BLE/TLS 互斥、重连退避、连接窗口、App/OTA/配网保护、恢复失败重试及 SDK 静默退出。
- `tests/mqtt_start_memory_test.py`：真实启动顺序、栈回收、完整 8 KB MQTT 栈、启动失败恢复、离线配网。等待常量从生产源码提取，不再硬编码 20 秒。
- `tests/ble_ota_memory_lifecycle_test.py`：真实 BLE 生命周期、暂停所有权、任务创建失败回滚、通知生产者隔离与恢复。
- 定向回归还覆盖 TLS 配置、远程 OTA 内存、HTTPS 续传、MQTT retained OTA 拒绝、OTA 重放保护和 DSP 读取/IAP 所有权。
- 独立构建目录 `build_mqtt_reconnect_20261019`；沿用上一版设备配置，未修改分区表或擦写设备。

这些测试与编译不证明现场射频/网络连接已经恢复，仍需实机验收。

最终本地验证：12 组定向回归全部通过（BLE 生命周期 9 项），ESP-IDF 5.5.4 编译/链接通过。应用 1,616,384 字节，分区余量 87,552 字节；与 0.6.36 分区表 SHA-256 相同，镜像校验有效。交付应用 SHA-256 为 `77a61441c085463ce5843235ff690bb4fde113b868d1c473e01eb53910f8ba47`。仅本地交付，未刷机、未提交/推送、未部署服务器。

## 实机验收

使用保留 SN、Wi-Fi、工厂参数的本地应用升级流程安装应用固件，不依赖当前不可用的云端 OTA；不要擦除整片 Flash，也不要在未知活动 OTA 槽时直接猜地址写入。

升级完成后先结束手机 BLE 会话，保持设备供电和网络稳定至少 60 秒，保留以下日志：

- `[DEVICE] ... firmware=0.6.37-mqtt-memory-fix`；
- `Startup stack released`、`Got IP`，随后才出现 `Starting MQTT after DHCP`；
- `TLS memory phase=task_start/before_connect/connected`，以及 `BLE paused for MQTT` / `BLE restored after MQTT window`；
- `DSP link=ONLINE`，03/04 累计增长，CRC/UART 错误不增长。

只有 `connected`、服务端收到该 SN 新遥测和订阅/OTA 状态独立核对，才能认定真实连接或升级链路通过。若仍超时，下一步采集设备解析出的 IP 与 broker 侧连接日志，不把本机 DNS 代理地址当作设备解析结果。
