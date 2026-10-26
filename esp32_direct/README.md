# ESP32-C3 直连控制器

本工程由 ESP32-C3 直接管理 DSP、BMS、ST7789 屏幕与按键，并提供 Wi-Fi、BLE、MQTT 和 USB 维护功能。目录由 `esp32_arm_replacement` 更名为 `esp32_direct`，构建项目名仍为 `cs_inv_direct_c3`。

当前源码版本见 [version.txt](version.txt)。这是持续联调的控制器工程；功能实现、主机测试、构建记录和实际设备验收分别记录。历史文档中的旧目录路径用于追溯当时产物，新命令应在本目录执行。

## 功能与边界

| 模块 | 当前源码包含的能力 | 使用边界 |
| --- | --- | --- |
| DSP | UART 遥测、参数持久化与同步、整组电池设置、直连升级 | 已保存或串口已提交不能证明 DSP 已采用配置 |
| BMS | RS485 遥测、心跳数据、按需身份查询、直连升级 | 默认 BBJ `0x7C` 协议；菜单中的协议编号不代表已适配所有厂商 |
| 显示与按键 | 中英文状态页、设置、升级状态、LCD 恢复与隔离诊断 | SPI 返回成功不能证明屏幕实际显示正确 |
| BLE/MQTT | 配网、信息读取、本地与远程控制、回执、重连和离线缓存 | 命令幂等缓存有容量与有效期限制，不承诺跨重启唯一执行 |
| OTA | ESP 应用升级、DSP/BMS 独立 UART owner 升级路径 | 不支持未焊接 ARM 的升级；目标 Boot 事务完成不等于版本回读或硬件验收 |
| 工厂维护 | USB 识别、身份写入/复核、锁定及 Device/Factory/Production 配置 | 工具检查与报告引用不能替代实际校准和出厂审核 |

电池设置由 LCD、BLE、MQTT 共用校验与修订号流程。原生类型为 AGM、富液铅酸、自定义铅酸、带通信锂电和自定义锂电；已有配置记录不匹配时停止同步，避免自动切回默认电池类型。详见[电池配置契约](docs/BATTERY_CONFIG_20261020.md)。

## 构建

### 通用 ESP-IDF 环境

使用 ESP-IDF 5.5.4，在已加载 IDF 环境的终端中，从仓库根目录执行：

```powershell
cd esp32_direct
idf.py menuconfig
idf.py build
```

首次 `menuconfig` 根据 `sdkconfig.defaults` 生成本地 `sdkconfig`。检查板级 GPIO、网络和身份选项后保存。CMake 固定目标为 ESP32-C3，并从 `version.txt` 读取版本。

默认输出为 `build/cs_inv_direct_c3.bin` 和 `build/cs_inv_direct_c3.elf`。BIN 是应用镜像，不能当作地址 0 的整片镜像使用。默认分区表见 [partitions.csv](partitions.csv)。

### Windows 配置构建脚本

`build.ps1` 使用本机固定的 ESP-IDF、Python、CMake、Ninja 和编译器路径。先检查并调整脚本中的安装路径，再生成基础 `sdkconfig`，然后选择配置：

```powershell
.\build.ps1 -Profile Device
# 或选择以下一种配置
.\build.ps1 -Profile Factory
.\build.ps1 -Profile Production
```

| 配置 | 身份与 USB 行为 |
| --- | --- |
| Device | 优先管理员持久 SN，其次工厂身份；无身份时使用开发默认 SN，USB 身份只读 |
| Factory | 允许物理 USB 工厂写入和测试；缺少身份时不联网 |
| Production | 读取工厂身份，USB 身份只读；缺少身份时不联网 |

脚本从基础 `sdkconfig` 生成对应 `sdkconfig.device`、`sdkconfig.factory` 或 `sdkconfig.production`，构建后检查固件资源预算。三种配置共用 `build/`，该目录只保留最后一次构建的配置结果；配置名不代表已完成量产放行。

## 默认板级连接

以下为 `sdkconfig.defaults` 的板级配置。接线方向以 ESP 为基准，详见[引脚与原理图依据](docs/ESP32C3_PINOUT.md)。

| 功能 | ESP32-C3 GPIO / 参数 |
| --- | --- |
| DSP UART0 | TX 1、RX 0，115200 8N1 |
| BMS UART1 → RS485 | TX 21、RX 20，默认 BBJ 9600 8N1，自动换向 |
| ST7789 320×240 | SCK 6、MOSI 7、D/C 9、RESET 10，4 线 SPI |
| LCD CS / 背光 | 外部保持有效，不分配 ESP GPIO |
| BS818A_2 按键 | CLK 2、DAT 3 |
| 蜂鸣器 | GPIO8，高有效 |
| CAN 收发器空闲 | GPIO4 固定高；当前 BMS 使用 RS485 |
| 日志与维护 | USB Serial/JTAG，GPIO18/19 |

GPIO2/8/9 涉及启动配置，GPIO9 同时是 LCD D/C。默认配置的 `CONFIG_BOOT_ROM_LOG_ALWAYS_OFF=y` 会在固件首次运行时永久设置 UART ROM 日志关闭 eFuse；编译不会烧写芯片，恢复旧固件也不会恢复该日志。执行固件前需核对[启动配置说明](docs/ESP32C3_PINOUT.md)。

## 验证

从本工程目录执行现有主机回归：

```powershell
.\tests\run_production_host_tests.ps1
.\tests\run_home_ui_tests.ps1
```

脚本依赖本机 Python、GCC 和 ESP-IDF 源码；先核对其中的路径。Python 测试可单独执行，例如：

```powershell
python tests/battery_command_test.py
python tests/mqtt_start_memory_test.py
python tests/lcd_recovery_test.py
```

2026-10-26 对 `fe5e03b` 快照的定向验证中，电池命令、MQTT 启动内存、LCD 恢复、工厂身份 V2 和固件预算测试通过；`cloud_command_replay_test.py` 因测试构建缺少 `direct_battery_validate` 链接而失败，全量回归不能标记为通过。该记录不包含此快照的完整固件构建或设备验收。

## 工厂与升级

- [命令行工厂工具](tools/README.md)：分段烧录、身份配置、复核、测试和锁定。
- [工厂 GUI](tools/README_GUI.md)：图形界面与交付包。
- [设备自动识别与工单](docs/DEVICE_PROVISIONING_V0634.md)：逐台身份、重启复核及 INFO。
- [DSP/BMS 协议依据](docs/TARGET_PROTOCOL_EVIDENCE.md)：升级目标、原协议与验证范围。

已有设备维护应保留 NVS 和工厂身份。整片 4MB 镜像会覆盖这些区域；应用 OTA 不更新分区表。迁移分区、首次生产和跨 Device/Factory/Production 配置更换应按工厂工具说明执行。

## 进一步阅读

- [BLE 分帧与本地控制](docs/BLE_P0_20261019.md)
- [远程任务、期限与回执](docs/REMOTE_CONTROL_P0_20261020.md)
- [MQTT 内存与重连](docs/MQTT_MEMORY_RECONNECT_20261019.md)
- [OTA 启动确认](docs/OTA_BOOT_CONFIRM_V0632.md)
- [LCD 恢复](docs/LCD_RECOVERY_20261022.md)与 [20 MHz 隔离测试](docs/LCD_ISOLATION_20MHZ_20261023.md)
- [迁移清单](docs/PORTING_PLAN.md)：历史迁移计划，能力现状需结合源码与后续记录核对。

本地 `sdkconfig`、生成配置、构建产物和发布包不纳入版本控制。Wi-Fi/MQTT 凭据、管理员密码、签名私钥和逐台工单应单独保管。默认配置和 OTA 验证策略仍需按实际交付环境核对，当前不能宣称已实现强制签名或逐台云鉴权。
