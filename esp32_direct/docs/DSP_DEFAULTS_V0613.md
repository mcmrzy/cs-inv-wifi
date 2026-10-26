# 原ARM默认参数启动 v0.6.13-dsp-defaults

2026-10-14，用户确认先用原ARM默认初始化设置让逆变器运行。

主页左上角网络状态：WiFi已取得IP且MQTT连接服务器时显示“网络在线”，
其余显示“网络离线”。英文为Network online/offline，主页详情视图同样显示网络状态。
DSP新鲜度仍控制测量有效性、告警反馈及可写保护，诊断日志继续显示DSP通信状态。

“已有有效参数”指ESP Flash中dsp_params命名空间的SN绑定NVS记录；启动同步至DSP。
不是从DSP自动读出并恢复的记录，供应DSP源码不提供完整三包持久参数读取。

包含网络界面改动的最终全量回归及Device应用固件编译已通过；相关日志随交付包提供。
渲染回归包含中英文、深浅主题，网络状态独立于DSP状态，整帧与16行分条渲染一致。

## 固件行为

启动读取SN绑定的DSP持久参数：已有有效记录直接保留；仅在NVS namespace或record键不存在时，
生成并保存原ARM默认00/01/02完整三包。保存成功后才启用RAM参数，排队00→01→02，再查询03/04。
因此之前没导入参数的板刷应用固件即可初始化，不需要另行set_dsp。
记录损坏、不同SN、存储错误均不自动替换，仍可使用管理员完整参数导入纠正。
DSP UART任务仍不写Flash、不格式化日志；没有新增任务和动态大缓冲。

这些参数来自原ARM源码初始化，未读出物理ARM保存值。已有自定义设置不被默认值覆盖。
手动启动、报警及其他configWord位完整保留原ARM默认，未强制改运行开关或伪造BMS数据。

## 本版首次默认值

| 参数 | 值 |
| --- | --- |
| 输出 | 230V / 50Hz |
| 电池类型 | 原ARM代码1（AGM配置） |
| 浮充 / 截止 | 54V / 40V |
| 最大市电 / 总充电电流 | 60A / 140A |
| 充电 / 输出优先级 | 原ARM代码3 / 1 |
| 转市电 / 返回电池电压 | 48V / 57.6V |
| 市电输入允许范围 | 90～280V |
| 转市电 / 返回电池 / 截止SOC | 50% / 95% / 5% |
| configWord / configWord1 | 0x1DDA / 0x0321 |
| 并机设置 | 原ARM代码1 |
| 发电机平衡使能 | 关闭（0） |
| 发电机启动 / 停止SOC | 20% / 80% |
| 发电机启动 / 停止电压 | 48V / 56V |
| 发电机额定功率 | 3000W |

来源：USERFLASH.c init_setting_param_L00F960_flash及L00B8DC_flash；
SysParamFlash_Init先清零SysParam，ReadSysParamFormEEPROM提供范围默认；
UsartDsp.c fill_cmd00/01/02_packet编码。测试提取实际原函数，独立组包与固件全部64字节对比。
未使用测试夹具中的6000W发电机值。

## 升级及设备验收

只刷device/app.bin，保留SN、WiFi和已有DSP参数NVS。整片4MB镜像会覆盖配置。
启动确认版本0.6.13-dsp-defaults。新记录应打印：

    DSP persistent params=READY
    DSP ARM defaults saved: output=230V/50Hz battery_type=1 utility_max=60A total_max=140A generator=OFF

确认DSP sync ready=1 durable=1，replies至少3，并观察DSP link=ONLINE及03/04持续增加。
与逆变器正常电源开关操作配合验证输出，再保持通电至少两分钟并重启ESP/DSP确认同步恢复。
已保存有效记录不会再出现defaults saved日志；dsp_params可查看实际使用的三包。

原源码模式位不是电池化学类型检测，首次采用的是原AGM默认设置；实际电池设置需后续核对。
bulk不属于这三包持久恢复；供应ArmComm.c未包含25 producer，单项控制确认依赖实际DSP扩展。
详见DSP_SYNC_V0612.md的源码混包复现。该旧文档中首次必须手工导入的要求由本版缺记录自动初始化替代。
构建和软件回归不等于实机已运行；未烧录，DSP/BMS直连OTA仍关闭。
