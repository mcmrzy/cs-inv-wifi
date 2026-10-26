# DSP 参数确认与 AC 输出控制：证据和联调准备

## 本轮结果

ESP32 已补独立的 DSP 配置观测接口和 USB 诊断。没有开放 AC 开关机，没有把保存/串口提交改称 applied。当前提供的 DSP 源码缺少相应确认协议，单改 ESP32 无法证明 DSP 已采纳目标设置。

## 原始源码核对

DSP 来源：`C:/Users/29538/Documents/xwechat_files/wxid_6n4stl4q5dzq22_9518/msg/file/2026-10/ArmComm.c`，SHA256 `af623ada04cb4165058b94fb942d22cbdbea5af26d580b15aae3b3fbb7b8a121`。

- 242–275：00/01/02 只接收并更新配置变量，置 RecvParamXX，没有参数值回读或写入确认回包。
- 276–419：03/04 回报状态和运行量。运行输出电压/功率不能反证某次配置写入已生效。
- 428–500：若干单项设置分支，没有已定义的整机 AC 开/关命令和错误/完成回执。
- 502–537：缺少三包参数时向 ARM/ESP 请求 00/01/02；收到全部三包后置 GetArmParamReady。ESP 看不到这个内部变量，不能以不再请求同步代替 ACK。
- 全文件没有 0x25 发送分支。现有 ESP 解析 25 是旧 ARM 协议兼容能力，不代表这份 DSP 支持主动回读。

ARM 来源：`C:/Users/29538/Documents/xwechat_files/wxid_6n4stl4q5dzq22_9518/msg/file/2026-09/App(10)/App/source/`。

- `ComDSP/UsartDsp.c:336–338`：25 是并机参数同步，注释明确铅酸模式最大充电/市电充电电流修改不一定同步。即使收到该帧，也不能将整套参数或某次命令标为 applied。
- `USERFLASH/USERFLASH.h:49` 的 manual_start_enable、`APP/SystemManage.h:599` 的 ManualStartEn 仅有定义；未找到足够业务使用链路、极性和启动前提。
- `Collector/Collector.c:770–776` 的 ManualSocket/ControlSocket 设置分支已注释，不能作为 AC 控制映射。
- `APP/SystemManage.h:353–354` 的 PowerSwitch/EpsSwitch 是状态位命名；`APP/SystemManage.c:215` 使用 RelayLoad 更新原 UI 输出状态。缺少匹配当前 DSP 的位赋值来源，新增诊断只输出原始状态字，不猜控制语义。

## ESP32 本轮变更

- `direct_config_observed_snapshot_wire()`：只复制成功解析的真实 25 数据；给出字段有效掩码、独立序号、64 位接收时间和年龄。保持旧 desired 配置读取接口语义。
- 保存用户设置、00/01/02 同步、03/04 遥测不会制造或刷新配置观测。
- import 和 DSP 重启/IAP 完成后旧观测失效；Boot/IAP 期间不提供可用观测。没有观测时返回 false，消费者不能使用输出结构。
- diagnostics 新增 `config_frame`，保存完整通过 CRC/长度检查的 25 帧。该原始帧是最后一次接收证据，附接收时间；不是当前有效或本次命令已生效的保证。
- UART owner 仅复制数据。原始帧格式化留在主循环；USB status 格式化留在 USB 任务。

USB 输入 `status` 后新增：

```text
[DSP_OBSERVED] available=0 source=25
```

若收到真实 25，则输出接收序号、64 位年龄和已知字段：

```text
[DSP_OBSERVED] available=1 source=25 seq=3 received_ms=100 age_ms=6900
[DSP_OBSERVED_PARAM] index=7 wire_raw=2200
[DSP_STATUS_RAW] received_ms=6000 fault=0x00000000 status0=0x0200 status1=0x0080 status2=0x0000 mode=0
```

以上只是格式示例，不是实机抓包。wire_raw 使用原 Collector/MQTT 控制参数单位，index=7 的 2200 对应 220.0V；valid=false 的字段不打印，合法零保留。

周期诊断新增 `DSP capture kind=25_OBSERVED` 原始字节；当前 DSP 没有 25 时观察 unavailable 是正常、准确的结果。

## DSP 侧需要明确的接口

下一步应在匹配现场硬件的完整 DSP 工程内完成以下定义和实现；不预占未经确认的命令码：

1. 能力/协议版本标识，说明是否支持参数回读、命令确认与 AC 开关；ESP 只按实际能力启用。
2. 参数请求关联标识和配置版本。ACK 区分接收、拒绝、已应用；确认返回实际采纳值及目标版本，包含范围/状态/并机限制的明确错误。
3. 独立参数回读路径。写入之后必须能读取实际应用值，不能回显收到的原始数据或仅置 RecvParamXX。
4. 明确 AC 控制操作、开/关极性、运行前提、故障/并机限制，以及由 DSP 真实内部状态生成的输出反馈位。失联或超时是结果未知，不自动重放旧开机命令。
5. 串口发送互斥或有界队列：参数 ACK 不得覆盖正在发送的 03/04 帧。原 ScibSendData 直接复用缓冲的风险必须由 DSP 发送机制解决。

完成这些接口后，ESP 才能把同一请求的 submitted 推进到 applied，并将 ac_on/ac_off 绑定到有证据的输出控制操作。

## 验证记录

- 实际 UART 运行时测试先红后绿：独立 observed 与 desired 分离；保存和 03/04 不刷新；CRC 错误不更新；import 失效；原始帧保留；Boot/IAP 门控。
- 真实 IAP owner 测试验证目标重启/升级结束后的旧配置观测失效。
- 49.7 天回绕测试先失败，再改为 64 位时间后通过；实际 USB formatter 覆盖无观测、合法零、未知字段、陈旧年龄、原始状态位和无 applied 声明。
- 全套 `tests/run_production_host_tests.ps1` 通过；64 位时间评审修正后，UART runtime、USB formatter、IAP owner、MQTT 去重和云端/BLE回归再次通过。
- 最终构建目录 `build_control_observation_20261020`；版本号保持原 `0.6.39-mqtt-ble-coexist`，仅为本地编译候选。

最终 app 大小：1,621,168 字节（`0x18bcb0`），app 分区剩余 `0x14350` 字节（约 5%）；SHA256：`ce85bf74ca10ee52424b3693e6072391611ae19b36a7532804d9d0247df5ec1e`。

未刷机、未连接真实设备验收、未发布。UART owner 的真实栈水位与持续运行仍需现场检查；主机水位 stub 不构成硬件栈裕度证据。保留已有未提交改动，本轮不提交/推送，不写长期记忆。
