# TLS 初始化内存分配失败修复记录

日期：2026-10-13。工程：`D:\CS_INV_WIFI\esp32_arm_replacement`。

## 故障与修改

设备日志在 `mbedtls_ssl_setup` 返回 `-0x7F00`，对应内存分配失败。
目前没有设备上的内部 RAM 总空闲量/最大连续块记录，不能据此区分
总量不足、碎片化与泄漏，也不能用本地测试宣称设备故障已经消失。

- 默认、当前、Device、Production、Factory 配置启用
  `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y`。保留 IN=16384、OUT=2048，支持原有
  HTTPS OTA 的 16KB TLS 记录。没有启用提前释放 TLS 配置/证书的选项。
- 启动诊断任务打印 READY/FAILED 后删除自身；启动卡住时仍每 2 秒输出
  阶段信息。空闲任务随后回收约 3KB 栈和任务控制块。
- MQTT 在 before_connect、connected、error、disconnected 事件记录
  `TLS memory phase=... free=... largest=... min=...`，单位为字节。
  统计范围为 `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`，对应 TLS 使用的内部 RAM。
  使用 WARN 级别保证生产固件也可见。

任务栈尺寸、BLE 常驻行为、BMS 协议和接线配置未调整。已保存修改前文件：
`tmp/tls_memory_before_20261013/`。原 `build/` 产物未覆盖。

## 本地验证

- 新增 3 项回归检查通过：五个构建配置保留接收能力并启用动态缓冲；
  执行真实启动诊断函数验证 READY/FAILED 退出、启动卡住时继续输出；
  执行真实内存日志函数验证能力范围和数值。
- `tests/run_production_host_tests.ps1` 全部通过，包括 OTA 路由、启动健康检查、
  UART、BMS、管理员命令、工厂工具和 236 个界面场景检查。
- ESP-IDF 5.5.4 构建及最后一次 reconfigure/build 均退出 0。
  最后一次构建日志没有编译错误或警告。
- 生成配置启用动态缓冲；链接映射确认 ESP-TLS 调用动态
  `__wrap_mbedtls_ssl_setup`，包含按需分配和释放 RX 缓冲的实现。

日志：`tmp/tls_memory_host_tests.log`、`tmp/tls_memory_build.log`、
`tmp/tls_memory_build_verified.log`。

应用镜像：`build_tls_memory/cs_inv_direct_c3.bin`。
版本保持现有 `0.6.3`，使用当前 `sdkconfig`（Device，启动诊断启用），
不是新发布版本或生产通用镜像。

- 大小：1,487,072 字节（0x16b0e0）。
- OTA 应用分区：0x1a0000，余量 0x34f20（约 13%）。
- SHA256：`a743769a542d7ac08e69cb515609f11574843cb3490016ee41bbb20831cb127a`。

## 设备验收待完成

本轮没有烧录、上传或部署。动态缓冲减少初始化和空闲期占用，收到
大 TLS 记录时仍需足够连续内存。

1. 使用与设备身份和板级配置一致的固件，抓取上电到 MQTT 首次连接的日志。
2. 连续多次断网/恢复，比较每次 `before_connect` 的 free/largest，
   确认没有逐次下降；min 是历史最低值，下降本身不代表泄漏。
3. 执行真实 HTTPS OTA，验证 16KB 记录下载、校验、重启与 MQTT 重新连接。
4. 保持 BLE、LCD、DSP/BMS 等正常业务运行，观察是否仍出现 `-0x7F00`。

日志中的 BMS `bytes=0` 是另一个尚未闭环的问题，此修复没有证明其恢复。
