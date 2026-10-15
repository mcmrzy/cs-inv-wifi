# ESP32-C3 模组引脚依据

依据用户提供的 ESP32-C3-WROOM-02UD-N4 原理图片段（[原图](esp32c3-module-pinout.png)）。ARM 未焊。网名是图纸上的标识，不能单靠网名判断收发器的电平、方向控制或显示器的全部连线。

| GPIO | 图纸网名 | 新工程使用 | 状态 |
|---:|---|---|---|
| 0 | `SCITX-DSP'` | DSP UART0 RX | ARM PA3 是 RX；网络从 DSP 端命名 |
| 1 | `SCIRX-DSP'` | DSP UART0 TX | ARM PA2 是 TX；网络从 DSP 端命名 |
| 2 | `KEY_CLK` | BS818A_2 时钟 | 已预设；启动配置脚，上电电平需验证 |
| 3 | `KEY_DAT` | BS818A_2 数据 | 已预设 |
| 4 | `ARM-BMS-CANTX` | 已接出的 CAN 网络 | 本次 BMS 走 RS485，未初始化 TWAI；重分配前核对电路 |
| 5 | `ARM-BMS-CANRX` | 已接出的 CAN 网络 | 本次 BMS 走 RS485，未初始化 TWAI；重分配前核对电路 |
| 6 | `SPI2_SCK` | TFT SPI SCLK | ARM PB13，4 线 8 位 SPI |
| 7 | `SPI2_MOSI` | TFT SPI MOSI | 已预设 |
| 8 | `BUZZ` | 蜂鸣器输出 | 默认高有效，应用启动最早阶段强制低电平关闭；同时是启动配置脚 |
| 9 | `SPI2_MISO`、`BOOT` | **TFT D/C（RS）** | ARM PB14 实际为 D/C 输出，命令低、数据高；与 BOOT 共享 |
| 10 | `RESET-TFT` | TFT RST | ARM PA1，低有效 |
| 18/19 | `ARM/ESP-USB-D-/D+` | USB Serial/JTAG | 留给日志、下载和维护 |
| 20 | `ARM-BMS-485RX` | BMS UART1 RX | 已预设，经 RS485 收发器；换向接法待核对 |
| 21 | `ARM-BMS-485TX` | BMS UART1 TX | 已预设，经 RS485 收发器；换向接法待核对 |

2026-09-30 根据 ARM 图与源码修正：`GUI/halLcd.h` 的 RS_L/RS_H 对应 PB14；`GUI/halLcd.c` 将 PB14 配置为普通输出、SPI 为 8 位。因此 SPI2_MISO 实际是屏幕 D/C，ESP GPIO9 必须驱动它。之前按 D/C 上拉推定 3 线 9 位的配置错误，已改为 4 线 8 位；上拉仅是默认电平，不能代替命令/数据切换。

## 完整功能映射

| 功能 | ARM 管脚 | ARM 图网络 | ESP GPIO |
|---|---|---|---|
| 控制器向 DSP 发送 | PA2 / USART1_TX | SCIRX-DSP' | 1 |
| 控制器接收 DSP | PA3 / USART1_RX | SCITX-DSP' | 0 |
| BMS TX / RX | PA9 / PA10 | ARM-BMS-485TX / RX | 21 / 20 |
| TFT SCK / MOSI | PB13 / PB15 | SPI2_SCK / MOSI | 6 / 7 |
| TFT D/C | PB14 | SPI2_MISO | 9 |
| TFT RESET | PA1 | RESET-TFT | 10 |
| TFT CS | PB12 | SPI2_NSS | 图中未接 ESP；用户最终确认下拉 |
| TFT 背光 | PA8 | LED-BACKLIGHT | 图中未接 ESP；用户确认拉高 |
| 按键 CLK / DAT | PB1 / PB0 | KEY_CLK / DAT | 2 / 3 |
| 蜂鸣器 | PB2 | BUZZ | 8 |
| CAN TX / RX | PB9 / PB8 | ARM-BMS-CANTX / RX | 4 / 5，当前未启用 |

ARM WriteComm/WriteData 都执行 CS_L，CS 是低有效。用户最终确认 CS 下拉、背光上拉，与原驱动有效电平一致；CS/BL=-1 表示由外部硬件保持有效，无需 ESP GPIO。ST7789V 电源、porch 和 gamma 配置已从原 halLcd.c 移植。

GPIO9 与 BOOT 共用。用户已确认屏幕 RS（标为 MISO）上拉为 10kΩ，之前依据早期图片提出的 100Ω 重负载假设不适用。应用运行后由 GPIO9 切换 D/C，不能按住 BOOT；GPIO9 的启动配置用途仍需保留。

C3 仅有两路 UART，DSP 与经 RS485 收发器连接的 BMS 已各占一路；维护通道使用 USB Serial/JTAG。GPIO4/5 虽标为 CAN 网络，但本次 BMS 路径是 GPIO21/20 的 UART→RS485；不启用 CAN 驱动。

原 ARM 的风扇、温度、发电机、智能开关仍无可确认的 ESP 连线。GPIO4/5 的 CAN 网络若改用于 CS 或背光，必须先核对和修改接线，不能仅修改 Kconfig。

GPIO20/21 在芯片 ROM 启动阶段是默认 UART0 RX/TX；进入应用后才分别接入 BMS UART1。ROM 日志影响的是 GPIO21 上的 BMS 线路，不能根据运行时 UART0 接 DSP 推断启动日志也走 DSP 引脚。

2026-10-13：按用户要求，所有构建配置使用 `CONFIG_BOOT_ROM_LOG_ALWAYS_OFF=y`，USB Serial/JTAG 保留为唯一日志控制台。ESP-IDF 在固件首次执行的系统初始化阶段永久烧写 `UART_PRINT_CONTROL=3`，只关闭 UART ROM 打印，不禁用应用 UART、BMS 通信或 UART 下载功能。恢复旧固件不能恢复 UART ROM 日志；首次执行时 ROM 已先运行，因此该次启动仍可能输出日志，后续复位/冷启动才静默。编译不会烧写芯片。

上述自动设置依赖芯片 `UART_PRINT_CONTROL` 尚为默认值0或已为3；所用 ESP-IDF 的 setter 对已为1/2的芯片返回错误，烧录前应读回该字段确认。GPIO2/8/9 的上电电平、RS485 自动换向、DSP/BMS 线路隔离和电平仍需实测。ESP32-C3 的两路 UART 见 [Espressif UART 文档](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32c3/api-reference/peripherals/uart.html)。
