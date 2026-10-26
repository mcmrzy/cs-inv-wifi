# CS Inv WiFi · 逆变器通信与直连控制固件

辰烁科技逆变器的 ESP32 通信固件、直连控制器及配套开发工具。仓库同时保留 ESP-IDF 和早期 Arduino 工程，编译前请先选择与硬件架构对应的目录。

当前直连控制器入口为 **[esp32_direct](esp32_direct/README.md)**，由原 `esp32_arm_replacement` 目录更名而来。固件版本以该工程的 [version.txt](esp32_direct/version.txt) 为准。

## 选择工程

| 目录 | 用途 |
| --- | --- |
| [esp32_direct/](esp32_direct/README.md) | ESP32-C3 直接连接 DSP、BMS、屏幕与按键，整合联网、本地控制和维护功能 |
| `esp32c3_l10_idf/` | L10 通信模块 ESP-IDF 工程，通过 ARM 中转连接逆变器 |
| `esp32c3_idf/`、`esp32c2_idf/` | 其他 C3/C2 通信模块工程，使用各自的配置和分区表 |
| `esp32c2_test/` | C2 测试工程 |
| `src/`、`platformio.ini` | 早期 Arduino / PlatformIO 通信模块工程 |
| [simulator_platform/](simulator_platform/README.md) | Web 设备认领、遥测与 MQTT 控制模拟平台 |
| `tools/` | 串口模拟器、协议测试、扫码绑定及其他辅助工具 |
| `docs/` | 通信协议、接口说明与设计资料 |

## 直连控制器能力

- **DSP/BMS 通信**：独立 UART 任务负责 DSP 遥测、配置同步及 BMS 数据采集；BMS 默认使用现有 BBJ `0x7C` 协议。
- **屏幕与按键**：ST7789 320×240 界面、中英文显示、状态与参数菜单、升级状态及 LCD 诊断。
- **网络与本地连接**：Wi-Fi、BLE 配网与本地控制、MQTT 上报、离线缓存和网络重连。
- **电池设置**：LCD、BLE、MQTT 共用整组电池配置校验、修订号和持久化流程，支持铅酸与锂电配置。
- **升级与维护**：ESP 自升级、DSP/BMS 直连升级路径、Device/Factory/Production 配置及 USB 工厂身份工具。

参数回执中的 `saved` / `submitted` 表示 ESP 已保存或已提交同步；DSP 实际采用需要独立回读证据。升级下载完成、编译通过和实机验收也分别记录，不互相替代。具体支持范围见[直连工程说明](esp32_direct/README.md)。

## 快速开始：ESP-IDF 直连工程

使用 ESP-IDF 5.5.4，并先在当前终端加载其开发环境。

```powershell
git clone https://github.com/mcmrzy/cs-inv-wifi.git
cd cs-inv-wifi/esp32_direct

# 首次配置，按实际硬件和连接环境调整并保存
idf.py menuconfig

# 编译
idf.py build
```

工程通过 CMake 指定 ESP32-C3，默认配置来自 `sdkconfig.defaults`，分区布局见 `partitions.csv`。默认配置包含首次运行时永久关闭 UART ROM 日志的 eFuse 设置；运行固件前请阅读[引脚与启动配置说明](esp32_direct/docs/ESP32C3_PINOUT.md)。

Windows 本机也可使用 `esp32_direct/build.ps1` 构建不同配置；脚本内的 ESP-IDF、Python 和工具链路径需要与本机安装位置一致。初始化配置、构建产物和维护流程见[构建说明](esp32_direct/README.md#构建)。

## 其他工程构建

L10 的 ESP-IDF 工程需在自己的目录内配置和构建。从仓库根目录执行：

```powershell
cd esp32c3_l10_idf
idf.py menuconfig
idf.py build
```

各工程的 GPIO、协议、版本和分区布局独立，配置不能直接混用。早期 Arduino 工程在仓库根目录使用 PlatformIO：

```powershell
pio run -e esp32-c3-devkitm-1
```

## 工具与文档

| 入口 | 内容 |
| --- | --- |
| [直连工厂工具](esp32_direct/tools/README.md) | 身份写入与复核、分段烧录、升级和测试流程 |
| [工厂工具 GUI](esp32_direct/tools/README_GUI.md) | 图形界面及打包说明 |
| [电池配置](esp32_direct/docs/BATTERY_CONFIG_20261020.md) | 电池类型、成组提交、NVS 兼容与同步边界 |
| [BLE 本地控制](esp32_direct/docs/BLE_P0_20261019.md) | 信息读取、分帧、会话协商与保存级回执 |
| [远程控制](esp32_direct/docs/REMOTE_CONTROL_P0_20261020.md) | 任务幂等、期限和命令阶段 |
| [DSP/BMS 协议依据](esp32_direct/docs/TARGET_PROTOCOL_EVIDENCE.md) | 目标协议与升级证据 |
| [L10 MQTT 协议](docs/CS-L10-6K2_MQTT_上报协议设计_V2.1.md) | L10 上报协议设计；其他工程需结合实际实现核对 |
| [ARM 串口协议](docs/ARM_ESP32_UART_Protocol.md) | ARM 中转架构的串口帧协议 |
| [BLE 配网协议](docs/BLE_Provisioning_Protocol.md) | 配网接口说明 |
| [扫码绑定工具](tools/web/README.md) | Web 绑定工具入口 |

历史设计和验证记录包含各自日期、版本及构建哈希，不代表当前源码已经通过同样的设备验收。首次烧录、分区迁移和已有设备维护应使用对应工具流程；完整 Flash 镜像会覆盖设备身份和用户配置，应用 OTA 不更新分区表。

## 配置与验证

Wi-Fi、MQTT 和身份设置按所选工程配置。管理员密码、连接凭据、私钥和逐台工单保存在本地，不应写入 README 或提交到仓库。`esp32_direct` 的生成配置、构建目录、临时文件和发布目录由 `.gitignore` 排除。

直连工程的主机测试位于 `esp32_direct/tests/`。测试入口、依赖与当前已知限制见[验证说明](esp32_direct/README.md#验证)。主机测试与固件构建之后，仍需分别验证真实 DSP/BMS、屏幕、BLE/MQTT 和断电恢复。

## 版权

Copyright © 辰烁科技。第三方组件及字体遵循各自目录中的许可说明。
