# 设备自动识别、工厂配置与 INFO（0.6.34 / 工具 1.3.0）

## 操作步骤

1. 解压完整上位机包，保留 `firmware` 目录，运行 `CSERGY_FactoryTool.exe`。关闭占用同一 USB 的串口工具。
2. 接入设备。只有一台 Espressif USB Serial/JTAG 候选时工具自动进行只读识别；多台同时连接时选择实际串口并点击“识别设备”。自动识别不烧录、不写配置。
3. 空白芯片或旧固件可能不响应工厂协议。点击“ROM 芯片识别”可主动进入下载模式，读取芯片、MAC、Flash 容量。此操作会复位选中的设备；ROM 识别不能判断板卡类型，完成后运行设备需要重新上电。
4. 首次生产配置选择 `factory` 固件目录，检查型号和版本，点击“备份并更新固件”。只有分区布局确实不同时才选择首次迁移。工具核对 ROM MAC、ESP32-C3 和 4MB Flash，在同一连接中先备份、再烧录及校验。
5. 重新上电，识别设备。在“身份工单”填写或导入整机和板卡资料，点击“写入身份”。上位机绑定本台 MAC，写后读回；本地台账阻止重复 SN 分配给其他 MAC。
6. 再次断电上电、识别，然后“复核身份”。只有运行 SN、持久 SN、启动修订号和持久修订号一致，且设备没有待重启状态时，才记为复核通过。
7. 按已有流程写入扫码 PIN、完成测试和报告审核，再单独锁定身份。联网后 INFO 自动上报配置；可用 `query_info` 请求补报。服务器的新字段保存和页面展示需单独联调。

`factory` 支持未锁定身份写入；`production` 提供生产运行与只读查询；`device` 是开发维护配置，优先使用管理员持久 SN，其次有效工厂身份，无身份时才使用开发默认值。不要把开发默认 SN 用于量产。锁定不会因换固件而解除。

## 工单字段

| 内容 | 字段 |
| --- | --- |
| 整机 | `sn`（16 位大写 ASCII 字母数字）、`product`、`model`、`hw_system` |
| 主控板 | `controller_type`、`controller_code`、`hw_controller`、`controller_sn` |
| DSP 板 | `dsp_type`、`dsp_code`、`hw_dsp`、`dsp_sn` |
| BMS 板 | `bms_type`、`bms_code`、`hw_bms`、`bms_sn` |
| 功率板 | `power_type`、`power_code`、`hw_power`、`power_sn` |
| 生产 | `batch`、`production_time`（UTC）、`work_order`、`station_id` |
| 检测引用 | `calibration_version`、`calibration_summary` |

板卡 `code` 描述一种板卡型号/编号，板卡 `sn` 标识其中某一块。缺失的板卡信息可以留空，不填假值；硬件版本未知时保持未设置，不能因此声称硬件适配已验证。只存报告引用，不写未知校准系数。

类型与硬件版本最多 15 个 ASCII 字符，板卡编号/SN/工单/工位/批次最多 31 个字符，报告引用最多 63 个字符。校验失败会明确停止，不截断。实时固件版本、MAC、芯片、摘要由设备读取。配置项不包含产品密钥、管理员密码或网络密码。

## 升级与兼容

- 身份 schema 1（336 字节）继续可读；显式工单写入采用 schema 2（736 字节），使用原 factory 分区双槽、CRC、MAC 绑定、修订号和锁定保护。老固件缺少扩展写入能力时，工具提示先升级。
- 常规分段烧录保留 factory 身份和用户 NVS；未知分区布局保存备份后停止。首次迁移会清除旧 cache 占用区，需人工明确选择。
- 单个 `app.bin` 写入项目 ota_0 并重置 OTA 选择，仍保留配置。完整 4MB 合并 BIN 覆盖整片，包括 SN/PIN/设置，界面会独立说明并确认；已有设备优先使用固件目录方式。
- 更新后先保持 Factory 进行配置和复核，正式发货可维护为 Production 配置。不要将 schema 2 身份设备回退到只认识 schema 1 的旧固件；旧固件无法读取新记录。

## INFO 报文

`cs_inv/<运行SN>/info` 保持原协议 v2 信封。新增 `chip`、`mac`、`factory_identity` 与 `boards`。`factory_identity` 包含状态、schema、持久和启动修订号、锁定、是否待重启、工单及检测引用。`boards` 按 controller/dsp/bms/power 包含 type/code/hardware_version/sn。

顶层 SN 始终使用运行 SN。写入尚未重启时，持久资料仅作为待生效配置上报，`pending_reboot=true`，不把新 SN 混入旧 MQTT 会话。无身份的字段与未知板卡属性为 `null`；DSP/BMS 固件版本和额定参数在没有实际协议证据时仍为 `null`。这些是设备上报的配置，不等于服务器已审核的硬件档案。

连接、重连、`query_info` 及配置修订变化触发 INFO。INFO 不进入容量较小的离线缓存；在线排队失败后间隔 5 秒重试，并在重连时补报。PIN、管理员密码、产品密钥不进入 INFO 或普通识别报告。

## 验收范围

交付中的主机测试、目标构建、文件 SHA256 分别作为软件证据。真实设备仍需验收 USB 插拔、多设备换机、备份/烧录/读回、配置中断电和恢复、重启复核、实际联网 INFO 接收、内存余量与稳定性。服务器接收后的完整落库、界面展示和审核台账不能由设备端报文测试代替。
