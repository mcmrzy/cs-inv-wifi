# 0.6.28 现场证据：首包 ACK、完整传输与应用应答

本记录基于本轮两份串口日志及本地 `Invt280039C2_1.1.50V.bin` 的只读核对。
未烧写硬件，未修改生产代码，未把附件中另一段分析文字当成实机工具输出。

## 首包实验已取得实际回复

在 `334726 ms` 记录到：

```text
IAP dsp diagnostic (14): rx=19 bytes head=48 52 01 10 04 38 00 02 04 00 00 00 11 03 D1 43 52 43 52
IAP phase=DATA_ACK tx_frames=1 tx_len=1029 tx_max_ms=90 rx=2 head=43 52
IAP tx_head=02 01 FE tx_crc=FD A9
```

19 字节可拆为 HR（2）+ 完整擦除 ACK（13）+ 擦除 CR（2）+ 首包 CR（2）。
本地文件重算第 0 包 CRC16 得到 `FD A9`，与现场日志相符。
因此首包实验已证明 ESP 提交了正确的首包帧并读到 DSP 协议 ACK。
CR 本身不是独立 Flash 读回证据。

此前/随后若干作业止于 `phase=HR rx=0`，它们没有进入擦除/数据阶段；
不能把这些握手失败与成功取得首包 CR 的那次作业混为一次。

## 最新完整传输后出现了新的有效应用帧

最新日志的实际时间线：

- `314106 ms`：暂存 DSP 镜像，45928 字节，头为 `0048 75C1 5A5A`。
- `320559 ms`：收到命令 04 的完整 69 字节新帧。
- `320952 ms`：`DSP link=ONLINE`，`04=1`，`age04_ms=91`。
- `321004 ms`：服务记录 `DSP application responded; firmware version/flash readback unavailable; unconfirmed`。

从实际带时间戳的 capture 行重组该帧，CRC16/Modbus 重算值为 `0x1B50`，
尾部低字节先为 `50 1B`，校验通过。附件后续说明中的引用行不参与这次重算。
这足以确认完整传输返回后 DSP 应用协议通信恢复；不能继续称该次“一个字节未发”。
不能只因 04 内容不同，就独立认定 DSP 固件版本；数据协议中没有当前可用的版本读回。

附件文字报告 DSP 侧修复了 Flash 分块和 EOT 后立即跳转。其源码/构建/烧写证据
在另一台机器，当前未直接核对；它与本次应用通信恢复的观察相符，但不在此冒充独立验证。

## 0.6.28 的未确认状态含义

`TARGET_RESPONSE_TIMEOUT`：传输返回完成后没有观察到新的目标应用应答。
`TARGET_VERSION_UNCONFIRMED`：已经有新的 DSP 应用应答，但缺少独立版本/Flash 校验读回。
`DIAGNOSTIC_ONLY`：首包实验结束，没有执行完整镜像升级。
这些不同于握手/擦除/数据 ACK 超时。321004 ms 那条日志不是新的传输失败。

当前只有一条新 04 样本，且其 raw 值（如 grid_V=514、output_W=32767）与之前
正常运行记录差异明显。CRC 正确不等于测量字段/物理运行已完成验收；需连续帧和现场值核对。

## 如需独立确认落盘，使用本次传输后的读回

本地 bin：45928 字节，45 包，末包数据 872 字节，末包索引 44，地址编码 `2DD2`。
文件 SHA256：`c1e16e3ae511161bd1b955adfe43cae745924bb9dae3cb8b6014055061e95e2c`。

若 DSP 按提供的每字高字节先格式写在 `0x082000`，前 16 个完整 16 位字应为：

```text
0048 75C1 5A5A A5A5 0000 59AA E0EC D373
7648 4D38 0006 7648 4D56 0006 761F 030E
```

本地样本长度字段为 `0x59AA`（22954 字），恰等于文件 offset 20 之后的字数；
其 offset 20..EOF 的 zlib CRC32 为 `E0ECD373`，与头字段相符。
这只是文件内部一致性，仍需对照当前 Boot 的实际头布局与 CRC32 实现。
不要再用早先未写入时的全 FF 读数代替这次传输后的 Flash 状态。

入口 `0048 75C1` 按 C28x LB 的 22 位地址字段解码为 `0x0875C1`，位于该文件
映射的 `0x082000..0x0879B3` 内。解码依据：
[TI 对 C28x LB 编码的说明](https://e2e.ti.com/support/microcontrollers/c2000-microcontrollers-group/c2000/f/c2000-microcontrollers-forum/927136/compiler-tms320f28035-what-s-the-opcode-of-lb-_c_int00-in-dsp2803x_codestartbranch-asm)。
入口落在文件范围内不能代替链接、向量、RAM 初始化或实际跳转验证。

## 另外两项独立现象

较早日志含 Brownout detector 触发，证明有一次欠压复位记录；不能据此认定后续 OTA
无应用应答也是欠压导致，因为后续有独立连续运行和作业时间戳。
另有 MQTT TLS 动态分配失败与离线缓存分配失败，是堆内存不足/分配受限的证据，
不等同于 DSP 首包失败。不能用禁用欠压检测或削减 HTTPS TLS 输入记录支持掩盖它们。

可重跑的只读核对脚本：`tmp/audit_0628_device_log.py`、`tmp/audit_0628_app_response.py`。
