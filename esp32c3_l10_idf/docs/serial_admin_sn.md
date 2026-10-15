# L10 USB 串口修改 SN / ARM App CS 0x09

## 使用

连接 ESP32-C3 的 USB Serial/JTAG 虚拟串口，在串口终端发送一行：

```text
set_sn <ADMIN_PASSWORD> H1CNA0013500001O
```

最后发送回车、换行或 CRLF。SN 为 1..16 位 ASCII 字母数字，大小写保留；不允许空格、斜杠、通配符或其他符号。控制台不回显输入、不记录管理员密码，终端软件自己的本地回显需自行关闭。支持退格和 Delete；输入超长或含非法控制字符时整行丢弃。

管理员密码由 `menuconfig → CS_INV L10 Module → USB serial administrator password` 配置，默认采用本次用户指定值；设为空则禁用此功能。它与 MQTT 密码独立。输入发生在 USB 维护口，密码不发送给 ARM。

`ACCEPTED` 只表示开始处理；最终 `OK` 必须满足 ARM 写应答成功、0x06 回读完全一致、ESP NVS 保存成功。之后 ESP 重启，以新 SN 初始化 MQTT client ID、主题和 BLE 名称。

## CS 协议合约

本页定义的是当前 TEA+CRC32+PPP 的 **CS Collector App 协议**，采用现有 `cs_frame_send` 编码。字段为小端，CmdLen 按 4 字节对齐，CRC32 位于 CmdLen 偏移；双层 TEA/CBC 和 PPP 转义沿用现有实现。

### ESP → ARM 写 SN

| 字段 | 值 |
|---|---|
| Header | 0xAA |
| CmdLen | 24（0x0018，含 8 字节头 + 16 字节数据） |
| CMD | **0x09** |
| Param1 | 0，写入完整 SN 区域，保留其他值 |
| Param2 | 16（字节数） |
| Data | 16 字节 ASCII，自然字节序，短 SN 右侧补 0x00；无额外第 17 字节 |

示例 SN `H1CNA0013500001O` 的解密帧头及数据（尚未添加 CRC、加密、PPP）：

```text
AA 18 00 09 00 00 10 00
48 31 43 4E 41 30 30 31 33 35 30 30 30 30 31 4F
```

这 16 字节对应 OnlyReadDataDef 的 `Inverter_SN1..4`，位于 52 字节只读信息的偏移 10..25。例如首 4 字节为 H/1/C/N，SN1 小端数值为 `0x4E433148`。不要交换 u16/u32 内部字节，不使用 BCD。

### ARM → ESP 写结果

| 字段 | 值 |
|---|---|
| Header | 0xAA |
| CmdLen | 12（8 字节头 + 1 字节状态 + 3 字节零填充） |
| CMD | 0x09 |
| Param1 | 0，与请求对应 |
| Param2 | 1，状态数据长度 |
| Data[0] | 0=成功，1=参数非法，2=ARM 忙，3=ARM 持久化失败；其他非零也按失败处理 |

ARM 必须在 SN 持久化成功并更新只读信息后才发送成功应答，不因写 SN 自动进入 Boot、不重启 ARM。建议处理在 800ms 内完成。成功应答的解密前置数据为：

```text
AA 0C 00 09 00 00 01 00 00 00 00 00
```

### 回读确认

收到上述成功应答后，ESP 立即发送 `CMD=0x06, Param1=0x0190, Param2=52`。ARM 按现有只读信息协议返回全部 52 字节。ESP 对偏移 10..25 的原始 16 字节与目标零填充 SN 做完整比较，完全一致才更新 ESP NVS。

每个应答等待窗口沿用现有 800ms 超时。失败不自动重复写 SN，操作者可重新执行命令。

## 与 OTA 的关系

ARM **Boot** 模式已有 `0x09` 固件信息查询，其 Param1 为目标 1/2/3；本功能只在 **App** 模式使用 `0x09 / Param1=0`。ESP 在 OTA 保留/下载/安装期间拒绝修改 SN；SN 操作期间同样拒绝新 OTA 保留。接收分发按 App/Boot 模式切换，SN 成功应答还严格校验 Param1、Param2 和 CmdLen。

如果 ARM 当前固件在 App 中已占用 0x09，需要在 ARM App 的命令分发中明确加入上述 SN 分支并保持 Boot 的固件查询分支。不能只更改 ESP 的命令常量便声称 ARM 已支持。

## ARM 接入示例

本仓库没有 ARM 源码。以下是供 ARM 侧加入 `InstructionProcess` 的处理逻辑，`ArmSaveSn16` 和 `CollectorReply` 是示意接口，需要连接到 ARM 现有持久化、CS 帧应答实现。成功提交必须保证旧 SN 或新 SN 可恢复，不能先破坏包含其他参数的存储区。

```c
/* App 模式下新增 case 0x09。所有参数检查基于已解密并通过 CRC 的帧。 */
case 0x09: {
    uint8_t status = 1;
    if (cmd->CmdLen == 24 && cmd->Param1 == 0 && cmd->Param2 == 16) {
        bool valid = false, padding = false, bad = false;
        for (unsigned i = 0; i < 16; ++i) {
            uint8_t c = cmd->Data[i];
            if (!c) { padding = true; continue; }
            if (padding || !((c >= '0' && c <= '9') ||
                             (c >= 'A' && c <= 'Z') ||
                             (c >= 'a' && c <= 'z'))) { bad = true; break; }
            valid = true;
        }
        if (valid && !bad) {
            if (ArmConfigurationBusy()) status = 2;
            else if (!ArmSaveSn16(cmd->Data)) status = 3;
            else {
                /* pack(1) 布局中 SN 连续 16 字节；保留其他字段。 */
                memcpy(&pOnlyReadData->Inverter_SN1, cmd->Data, 16);
                status = 0;
            }
        }
    }
    CollectorReply(0x09, 0, 1, &status);
    break;
}
```

如 ARM 已存相同 SN，建议跳过重复 flash 写入，仍返回 0，以便 ESP NVS 失败后重试恢复。

## 结果与恢复

| 串口结果 | 行为 |
|---|---|
| AUTH_FAILED / INVALID_ARGS / LINE_TOO_LONG | 不发送 ARM 帧、不修改 NVS |
| BUSY | OTA、ARM 写入/轮询或另一条 SN 操作进行中；稍后重试 |
| ARM_REJECTED | ARM 返回非零；ESP 身份保持原值 |
| ARM_TIMEOUT / READBACK_MISMATCH / UART_TX_FAILED / INTERRUPTED | ESP 身份保持原值；ARM 可能已写入，需检查并重试目标 SN |
| NVS_FAILED | ARM 回读已确认新 SN，但 ESP 保存失败；ESP 身份保持原值，重新发送同目标 SN 恢复 |
| OK | ARM 回读一致、ESP NVS 成功；清理旧 retained status 并重启 |

开始管理员 SN 事务后，本次运行暂时关闭来自普通 0x06 的自动 SN 身份覆盖；失败后继续保留该保护，避免迟到应答绕过失败结果触发重启。认证重试成功会按新 SN 重启；手动重启后恢复原有 ARM SN 自动同步逻辑。因此，两端写入不是跨设备原子事务。

## 验证

宿主测试覆盖从 USB 输入的完整命令到真实 CS 编解码/轮询调度、ARM 应答和回读再到 ESP 保存的链路（USB、UART 传输、OTA 预留、NVS 平台边界为替身），以及密码和行解析边界。Windows：

```powershell
cmd /c D:\CS_INV_WIFI\esp32c3_l10_idf\tests\host\run_windows.cmd
```

实物验收：先给 ARM App 加入 0x09 分支，再分别验证错误密码、合法写入、ARM 断线、非零状态、短 SN 零填充、重启后两端持久化一致、MQTT/BLE 身份更新、OTA 与 SN 双向忙碌拒绝。宿主测试和 ESP 构建不证明 ARM 已集成或实物持久化通过。
