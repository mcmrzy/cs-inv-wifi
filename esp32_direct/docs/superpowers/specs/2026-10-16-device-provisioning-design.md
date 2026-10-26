# USB 自动识别、板卡配置与 INFO 上报

用户已批准：ESP32 直控版，在现有工厂上位机扩展自动识别、固件烧录、配置写入、重启复核及联网 INFO 上报。常规维护保留身份与用户设置，写入和锁定仍遵循 Factory 权限。

## 身份与协议

保留现有 schema 1（336 字节）身份读取。新增 schema 2（736 字节），前 312 字节字符串布局不变，扩展字段附加在原 CRC 前：controller_type[16], controller_code[32], controller_sn[32], dsp_type[16], dsp_code[32], dsp_sn[32], bms_type[16], bms_code[32], bms_sn[32], power_type[16], power_code[32], power_sn[32], hw_power[16], work_order[32], station_id[32]。原 hw_controller/hw_dsp/hw_bms 是各板硬件版本，hw_system 为整机硬件版本。空扩展值不代表已识别硬件。新工单 SN 为 16 位大写 ASCII 字母数字；旧记录仍可读取。

保留双槽、CRC、MAC 绑定、单调 generation、粘性锁定；断电/损坏槽回退到有效槽，相同 generation 的实际内容冲突拒绝。schema 1 转 schema 2 只在显式写入时发生；锁定记录不能因格式升级而绕过保护。

USB 帧版本保留 1，最大 payload 扩大为 4096；识别回复 factory_api=3、identity_schema=2、chip=esp32c3、provisioning_identity_v2=true。旧设备保留读取、诊断和维护能力；扩展工单写入要求新能力，不对旧设备发送 schema 2。记录使用现有 command 2，识别 command 1。

## 上位机

定时枚举 USB 端口，仅对 Espressif USB Serial/JTAG VID 303A/PID 1001 候选或用户主动选择端口进行只读握手，不对任意串口广播。单个新候选可自动选择并识别，多台要求用户选择。端口移除立即清除身份权限与页面旧状态；串口同名复用须重新识别；任务执行和诊断期间不抢占串口。

配置页面支持全部字段导入、导出及从设备读取。操作绑定预期 MAC，每次写入前重新查询实际 MAC，不匹配拒绝。SN 写后读回仍标记等待重启；复核必须同时满足 MAC、工单、runtime_sn、记录 generation，并检查固件提供的待重启状态。旧固件若无复核证据明确提示需更新。重复 SN/MAC 维持本地台账保护。

空白芯片/旧固件无 USB 工厂握手时显示未配置或不支持；用户主动执行 ROM 查询才进入下载模式，读取芯片/MAC/Flash；绝不把 USB 描述推断为板卡身份。烧录前核对 MAC、镜像芯片、大小、型号、profile 和摘要，先备份，分段烧录和读回；明确的完整镜像覆盖路径仍单独确认。

## INFO

保留原协议字段；sn 始终为运行 SN。仅当有效身份 SN 与运行 SN 一致时使用其中 model/hw_system，附加 factory_identity 对象（schema/status/generation/locked/runtime_match/pending_reboot/sn/product/model/batch/production_time/work_order/station_id/calibration_version/calibration_summary）与 boards 数组（role/type/code/hardware_version/sn），芯片 MAC 和真实运行固件。未配置/未知值为 null，不制造 DSP/BMS 固件版本、额定值或物理识别证据。敏感 PIN、密码、密钥不进入 INFO。

联网、重连和 query_info 经原 publish_info 路径发布；身份修订变化触发补报。写入后待重启时上报 pending_reboot，不把新 SN 作为旧 MQTT 会话身份。INFO 是设备声明，服务器审核台账独立；本次验证固件报文，服务器新字段入库/部署需要独立核验。

## 验收

主机验证涵盖跨语言编解码、旧记录迁移、断电/损坏回退、同代冲突、锁定、超过旧帧长度的合法 JSON、SN 格式、换机 MAC、重复 SN、运行与持久身份不一致、端口插拔多设备、INFO 缺失/转义/最长字段。构建 Device/Factory/Production 固件并生成工具和固件交付包（如环境支持）。真实插拔、烧录、断电、联网 MQTT 和服务器落库属于实机/部署验收，不能用主机测试替代。
