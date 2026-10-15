# 0.6.15-mutexfix：MQTT 设置上报导致重复加锁重启

## 实机证据

用户提供的 0.6.14-controls 日志在约 10 秒 MQTT connected 后反复出现：

```
assert failed: vTaskPriorityDisinheritAfterTimeout tasks.c:5270
(pxTCB != pxCurrentTCBs[ 0 ])
```

日志 ELF 前缀 15f61b63f 与实际交付 ELF 的 SHA256
15f61b63fc54abc78a32254093517ae24be243b68af9658ab481375d8beaa866 相符。
通过该 ELF 解码的关键调用路径是：

| 地址 | 对应函数 |
|---|---|
| 0x4038ee7e | vTaskPriorityDisinheritAfterTimeout |
| 0x4038d1bc | xQueueSemaphoreTake |
| 0x4200d98a | arm_poll_take_param |
| 0x4200da04 | direct_config_get |
| 0x4200da7e | direct_config_get_wire |
| 0x4201a0a6 | publish_config，被内联到 telemetry_tick |

旧版 telemetry_tick 持有参数互斥锁时调用 publish_config。
0.6.14 的 publish_config 又通过 direct_config_get_wire 申请同一非递归锁，
触发 FreeRTOS 自身持锁任务超时后的断言。这是应用调用链的问题。
本次日志已经确认 WiFi 获得 IP、MQTT TLS 连接成功，未出现 TLS 分配失败。

同一份日志中 DSP 在约 1.8 秒已回复 CRC 有效的 03/04，
sync ready=1、durable=1，UART 硬件错误为 0。
这是启动阶段通信成立的证据；反复重启导致无法验证持续通信和逆变器运行。
BMS 无回复仍是独立问题。

## 修改

- 新增一次性云端设置快照：42 个值、有效字段掩码、版本号在同一锁内读取。
- telemetry_tick 释放参数锁后才调用快照接口，并在锁外构造和发送 MQTT JSON。
- info 上报也先复制再解锁发送，避免持有参数锁调用网络发送。
- 互斥锁暂时不可用时，保留设置请求供下次 tick 重试。
- 重连或主动查询即使设置版本未变化，也重新上报当前保存设置。
- 保留真实 0，省略未知字段，优先使用 ESP 已保存的用户参数。

00/01/02 周期同步、保存后优先发送三包、不可修改默认包、六键操作、网络状态、
WiFi/TLS 配置、UART 引脚及任务栈设置沿用 0.6.14。
修复只在独立 0.6.15 快照构建，未混入共享工作区的其他开发内容。

## 验证和刷写

新增测试编译实际 telemetry_on_mqtt_connected、telemetry_request_config、
telemetry_tick、publish_config、参数 getter/快照和锁函数。
测试用非递归锁断言模型在修改前复现重复加锁，修改后验证首次连接、重连、
保存后上报、主动查询、两处锁竞争重试，以及真实 0、单位和版本一致性。
实际构建、完整回归和包装校验结果以 validation 目录记录为准。

更新已有设备只刷 device/app.bin，保留 SN、WiFi 和用户设置。
整片 4MB 镜像会覆盖持久配置。
烧录后应看到 firmware=0.6.15-mutexfix，MQTT 连接后不再发生上述断言，
并观察至少几分钟 DSP 03/04 计数持续增长。
再从云端、BLE、按键分别修改受支持设置并重启核对保存结果。
宿主回归和构建不代表已经完成实机持续通信或运行验收。
