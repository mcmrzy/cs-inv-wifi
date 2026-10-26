# DSP 控制与独立观测实施

用户要求继续 ESP32 远程控制。沿用 superagents/develop-feature 流程；本轮以原 ARM 和 DSP 源码核对为前置，不刷机，不新增未经确认的输出控制命令。

## 已确认的约束

- 所给 ArmComm.c 没有 0x25 参数回读发送；00/01/02 只有接收及缺参请求。
- 原 ARM 的 0x25 属于并机参数同步；并非所有配置均可观察，不能作为通用命令 ACK。
- manual_start_enable 只见定义和批量参数位；ManualSocket/ControlSocket 写路径注释，不能映射为 AC 开关。

## 本轮可验证目标

1. 独立读取真实 0x25 观测快照，返回原始接收时间、序号及年龄；绝不从 ESP desired store 补值。
2. 保存完整 CRC 有效 0x25 原始帧，由消费者打印，UART owner 不格式化/不发网络回调。
3. USB status 输出观测存在性/年龄/序号与已知字段；没有观测则显示 unavailable；03 状态只打印有来源的原始位，不自行推断开关极性。
4. 本地保存、03/04 遥测、重复同步均不得制造或刷新配置观测；import/目标重启使旧观测不可用。

## Roles / Ownership

| session | role | owned paths | deliverable |
| --- | --- | --- | --- |
| root | implementer/verifier | direct_poll.c、direct_link.h、factory_service.c、runtime tests、文档 | 红绿证据、构建、原始证据说明 |
| dsp_wire_evidence | researcher | none，原 DSP 和 ESP 只读 | DSP 收发命令及开关证据 |
| arm_control_evidence | researcher | none，原 ARM 只读 | 参数同步和状态位证据 |

## Tasks

- [x] 在实际 UART owner 提取测试增加独立观测断言，先确认失败。
- [x] 实现 getter、有效帧捕获、诊断输出，保留已有 desired 配置接口。
- [x] 通过运行时测试、云端/BLE控制回归和 ESP-IDF 构建；最终 64 位时间修正后的增量构建也已通过。
- [x] 写清 DSP 端必须补充的命令确认和开关语义，现有固件保持 submitted。

评审修正：观测接收时间和年龄改为 64 位，跨 2^32ms 红绿验证通过；实际 IAP owner 生命周期已有旧观测失效断言。

验收仅为软件行为与编译，不代表 DSP 新协议、输出开关或物理设备验收完成。
