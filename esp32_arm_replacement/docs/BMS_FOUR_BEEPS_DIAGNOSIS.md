# BMS 通信接入后四声：按键输入诊断

日期：2026-10-13。

## 最新实测条件

用户确认：一开始通信就响四声，每隔约一秒一声，第四声后停止；仅连接 BMS，无 DSP 和充放电；ARM 和 ESP 复用同一个 RS485 收发器，仅更换 TX/RX。500 ms 轮询对照固件仍会鸣叫，因此该调整未消除故障。此前“通信几分钟后才响”的描述已被用户更正。

## 本次直接核对的 BMS 工程

`C:\Users\29538\Documents\xwechat_files\wxid_6n4stl4q5dzq22_9518\msg\file\2026-10\App\App`

- `app/PC_485_protocol.C:52-89,1136-1149`：正常 `0x7C/0x01` 查询读取并回复电池信息，没有蜂鸣调用。
- `app/key_task.c:25-53`：`LONG_PRESS_START` 发出一声 60 ms 提示音；持续长按每隔 1000 tick 增加计数，仅计数小于 5 时鸣叫。因此长按序列为四声，随后停止。
- `bsp/board/board.H:168-170`：该按键为 **BMS 自己的 PA0**，高电平有效。它与 ARM 控制器的 PA0 是两个不同芯片的引脚，不能仅凭同名认定连接。
- `components/include/multi_button.h:8-11`、`components/multi_button.c`：20 ms 采样、长按门限约 1 s。
- `bsp/board/board.c:181-184`：PA0 初始配置为下拉输入。
- `app/key_task.c:94`、`app/led_task.c:333`：后续重新调用 `pin_mode(WAKEUP_PIN, PIN_MODE_INPUT)`。
- `bsp/libraries/drivers/drv_pin.c:148-164`：上述模式最终设置 `GPIO_NOPULL`，移除内部下拉。外部电阻和实际电平尚未核实，不能据此断言硬件引脚悬空。
- `app/Buzzer_task.c:4-15`：初始化提示为一声 150 ms，不能直接解释约一秒间隔的固定四声。
- `app/fixture/fixture_rs485_protocol.c:55-85,1190-1200`：治具蜂鸣路径要求 `0x2B` 头和正确 CRC；注册初始化导出被注释，不能把正常 `0x7C` 查询当成治具蜂鸣指令。
- 提供的 `MDK-ARM/release/Listings/BMS_Project.map` 包含 `button_callback`、`init_button_task`、`buzzer_task_init` 及其初始化项；但尚未核验实机 BMS 固件是否与此工程产物完全一致。

## 执行原代码的模拟结果

宿主诊断程序位于 `tmp/bms_key_diagnosis/`，直接包含原始 `key_task.c` 并编译原始 `multi_button.c`，替换硬件访问为模拟输入。未编辑 BMS 或 ESP 生产源码，未烧录设备。

| PA0 模拟输入 | 原代码行为 |
| --- | --- |
| 持续低电平 8 s | 不鸣叫 |
| 持续高电平 8 s | 1080、2080、3080、4080 ms 各鸣叫 60 ms；随后停止 |
| 高电平至 4200 ms 后变低 | 同样四声；释放时走恢复默认参数、AFE 重置、MCU 复位分支 |

该模拟证明长按分支可以产生用户描述的节奏，不证明 ESP 实际使 PA0 变高。源码可解释蜂鸣模式；电气触发仍未闭环。

## 下一步测量

优先同时记录 BMS PA0 对 BMS 本地地的电压、第一条 UART 请求和蜂鸣起点。分别比较未接控制器、接 ARM、接 ESP 三种条件。

若仅接 ESP 时 PA0 连续为高超过约一秒，再核对外部下拉、唤醒按键连接、TX/RX 与唤醒网络的串接/耦合及共地电位。不要在无原理图的情况下把未知网络直接短接到地。

若蜂鸣期间 PA0 始终稳定低电平，该长按假设不成立；需核验实机 BMS 固件版本，并检查是否存在复位或工程外的蜂鸣代码。逻辑分析仪记录收发器 TTL 侧 TX/RX，比较 ARM、ESP 第一条线上实际帧，不能仅凭软件组帧相同判定完整链路相同。

## 结论边界

“BMS 误判唤醒键持续按下”仅为鸣叫节奏吻合的候选解释，不是已证实原因。用户明确 ESP 经 RS485 接入；随后提供的 ESP 模组原理图没有建立其 UART 与 BMS 唤醒 PA0 的连接，不能凭四声就把 PA0 定为根因。软件中内部下拉被后续初始化移除是可核对事实，是否影响实机取决于外部电路和实际电平。500 ms 轮询调整已被用户实测为未消除蜂鸣。
