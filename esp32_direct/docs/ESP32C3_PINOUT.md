# L10 新板 ESP32-C3 引脚

依据新板 ESP32-C3-WROOM-02UD-N4 原理图片段，收发方向以 ESP 为基准。

| GPIO | 图纸网名 | 固件用途 |
| ---: | --- | --- |
| 0 | `SCITX-DSP'` | DSP UART0 RX |
| 1 | `SCIRX-DSP'` | DSP UART0 TX |
| 2 | `KEY_DAT` | BS818A 数据输入 |
| 3 | `KEY_CLK` | BS818A 时钟输出 |
| 4 | `TFT-RES` | ST7789 复位，低有效 |
| 5 | `TFT-BL` | 背光使能，固件按高有效处理 |
| 6 | `TFT-SCL` | SPI SCLK |
| 7 | `TFT-SDA` | SPI MOSI |
| 8 | `TFT-DC` | SPI D/C，命令低、数据高 |
| 9 | `LED_DAT` | WS2812 DIN，板上 10 kΩ 上拉 |
| 10 | `BUZZ` | 有源蜂鸣器使能，高有效，默认持续 150 ms |
| 18 / 19 | `ARM/ESP-USB-D-` / `ARM/ESP-USB-D+` | USB Serial/JTAG |
| 20 / 21 | `ARM-BMS-485RX` / `ARM-BMS-485TX` | BMS UART1 RX / TX，经 RS485 收发器 |

TFT CS 沿用板上拉低接法。GPIO5 在上电初始化和显示恢复期间保持低，屏幕使能后拉高。自动息屏同时关闭背光，按键、告警或升级唤醒时重新打开。GPIO4 已分配给 TFT 复位，默认禁用旧板的 CAN 空闲输出。BMS 继续使用原 RS485 自动换向接法。

触摸键 KEY1 至 KEY6 依次为确认、右、下、上、左、返回。正面左侧三键所用通道分别是 KEY6（返回）、KEY5（左）、KEY4（上）。

## WS2812 状态灯

DIN 起依次为 RUN、CHG、NET、ALM，使用 GRB 和 24/255 默认亮度。可通过 `CONFIG_DIRECT_LED_BRIGHTNESS` 调整。

| 灯 | 状态 |
| --- | --- |
| RUN | 待机黄呼吸、预充黄常亮、市电带载绿常亮、离网带载蓝常亮 |
| CHG | 充电绿呼吸、放电蓝呼吸、满电绿常亮、有电池且无电流蓝常亮、电池受限黄呼吸 |
| NET | BLE 已连接蓝常亮、云已连接绿常亮、配网蓝呼吸、重连红呼吸、仅 Wi-Fi 黄常亮 |
| ALM | 故障红常亮、告警或 DSP 数据超时黄常亮 |

RUN 使用 L10 DSP 的负载继电器、预充和市电状态。CHG 使用 L10 的实测充放电电流。DSP 数据超时时关闭依赖相应数据的运行和电池指示，保留最后一次收到的故障。

## 启动配置

GPIO2、GPIO8、GPIO9 是启动配置脚。GPIO9 的灯带输出在应用启动并通过引脚校验后才启用。

ROM 在应用启动前将 GPIO20/21 用作默认 UART0 RX/TX，可能在 BMS 线上输出启动日志。应用运行后这两个脚属于 BMS UART1。默认构建设置 `CONFIG_BOOT_ROM_LOG_ALWAYS_OFF=y`，固件首次运行时永久烧写 `UART_PRINT_CONTROL=3`，后续上电关闭 UART ROM 打印。固件仍可使用两路 UART 和 USB Serial/JTAG。首次运行前需确认 eFuse 读取值为 0 或 3，ESP-IDF 对已写为 1 或 2 的值会返回错误。

旧板引脚记录参见 [原理图片段](esp32c3-module-pinout.png)。
