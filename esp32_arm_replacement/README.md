# ESP32 替代 ARM：独立实验工程

这是一个新的 ESP-IDF 工程，不修改现有 `esp32c3_l10_idf`、ARM App、ARM Boot 或 BMS Boot。目标是让 ESP32-C3 直接管理 DSP、BMS、屏幕、按键及原 L10 的联网功能，去掉 ESP↔ARM 中间串口。当前版本是**硬件联调原型**，不能作为逆变器生产固件。

## 当前已实现

- 从 `esp32c3_l10_idf` 复制 Wi-Fi、BLE 配网/本地控制、MQTT、离线缓存、调试日志、SNTP 和 ESP 自升级代码，作为迁移基线。新目录有独立 CMake 工程、版本号、分区表和 Kconfig。
- DSP UART 直连：TX=GPIO1、RX=GPIO0、115200 8N1；根据 ARM PA2/PA3 的实际方向配置，网名从 DSP 端命名。生成 `0x03`～`0x06` 轮询帧，校验并解析主要遥测量。
- BMS UART1（GPIO21 TX / GPIO20 RX）经板上 RS485 收发器直连：9600 8N1；默认 `0x7c` 私有遥测协议，与所附 ARM App 默认 `RS485Protocol=5` / `BBJ_BMS_PROTOCOL=5` 对应。Modbus `0x03/0x0000/0x0025` 可在 Kconfig 中切换。检查长度、地址、CRC、单体数上界；只有收到新鲜 BMS 数据才向 DSP 发送 `0x05/0x06`。
- 蜂鸣器 GPIO8：按原 ARM 高电平有效逻辑，应用启动最早阶段强制输出低电平关闭；GPIO8 同时是 ESP32-C3 启动配置脚，上电瞬间电平仍需结合硬件极性验证。
- BMS Boot 的 `0x5A … 0xA5` 封帧、校验和 15 字节回包仅实现了纯编解码，用于后续直连升级开发；尚未连接运行时 OTA。
- ST7789V 320×240 SPI 只读状态页：按 ARM 源码默认使用 4 线 8 位 SPI，SCK6/MOSI7/D/C9/RST10；移植原电源和 gamma 初始化。BS818A_2 按键可翻页。
- 对云端/本地发来的非 ESP 目标 OTA 明确返回 `INVALID_TARGET`；没有 ARM 后，旧 ARM 中转的 DSP/BMS 升级流程不能再调用。
- 独立 `tests/protocol_test.c` 可在 Windows/MSVC 上验证基础帧编解码与边界。

## 板级接线

用户提供的 ESP32-C3-WROOM-02UD-N4 原理图片段已用于预设 DSP GPIO0/1、BMS 485 GPIO21/20、BS818A_2 按键 GPIO2/3，以及 TFT 的 SCK6、MOSI7、RST10。ARM 未焊。2026-10-13 用户确认屏幕 NSS 已独立硬件下拉，不再需要 GPIO4；从0.6.4起 LCD CS=-1，GPIO4 固定输出高，释放仍连接 BMS 的 U30 CAN 收发器。完整逐脚依据与尚未确认的信号见 [C3 引脚表](docs/ESP32C3_PINOUT.md)。GPIO2 是启动配置脚，按键电路须按原理图验证上电电平；GPIO8/9 也涉及启动配置，不能随意重新分配。参见 [Espressif GPIO 文档](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32c3/api-reference/peripherals/gpio.html)。

DSP 用 UART0，BMS 用 UART1，USB Serial/JTAG 用于日志与维护。ARM PB14 虽标为 SPI2_MISO，实际是屏幕 D/C，ESP 对应 GPIO9；D/C 上拉不能作为 3 线接口的依据。用户最终确认 CS 下拉、背光上拉：与原驱动的 CS 低有效、PA8 背光高有效一致，CS/BL=-1 使用外部固定有效电平。RS485 自动换向使用 DE=-1，GPIO 控制时填写 DE/RE 引脚。

## 验证

主机协议测试：

```powershell
cd D:\CS_INV_WIFI\esp32_arm_replacement
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 >nul && cl /nologo /W4 /WX /I main\direct tests\protocol_test.c main\direct\direct_protocol.c main\direct\direct_lcd_codec.c /Fe:protocol_test.exe && protocol_test.exe'
```

ESP-IDF 固件构建（使用本机已有的 5.5.4 环境）：

```powershell
cd D:\CS_INV_WIFI\esp32_arm_replacement
.\build.ps1
```

已用本机 ESP-IDF 5.5.4 / Python 3.13.14 完成 ESP32-C3 固件构建，并通过主机协议测试。尚未刷机、接 DSP/BMS、验证屏幕实物或 MQTT/BLE 端到端；构建成功只证明源码与当前配置可编译。

## 重要差距

详见 [迁移清单](docs/PORTING_PLAN.md)。尤其是 84 个配置索引的读写与同步、完整状态/能量/风扇数据、原屏幕菜单、风扇/蜂鸣器/发电机及智能开关控制、DSP/BMS 直连 OTA 的最终 ACK/CRC 验证。`arm_poll_*` 名称目前只是兼容旧 L10 调用方的过渡接口；`arm_poll_send_write()` 拒绝写入，不能误认为云端控制已生效。L10 的联网代码已复制并在 C3 目标下编译，但尚未经新板硬件回归。
