---
description: "ESP-IDF 编译环境: 设置 toolchain PATH 并执行 idf.py 命令 (build/flash/monitor/clean/erase-flash)"
---

# ESP-IDF 编译环境命令

自动设置 ESP-IDF v5.5 RISC-V toolchain 环境变量, 然后执行指定的 idf.py 子命令。

## 环境配置

以下路径是本机固定路径, 无需每次手动拼写:

- **IDF_PATH**: `D:\OpenSiFli\xiaozhi-sf32\esp32\ble_gateway\esp-idf\esp-idf`
- **Python**: `C:\Users\29538\.espressif\python_env\idf5.5_py3.13_env\Scripts\python.exe`
- **idf.py**: `$IDF_PATH\tools\idf.py`
- **Toolchain**: `C:\Users\29538\.espressif\tools\riscv32-esp-elf\esp-14.2.0_20251107\riscv32-esp-elf\bin`
- **CMake**: `C:\Users\29538\.espressif\tools\cmake\3.30.2\bin`
- **Ninja**: `C:\Users\29538\.espressif\tools\ninja\1.12.1`

## 用法

用户输入: `$ARGUMENTS` — 格式为 `<action> [port] [project_dir]`

- `action`: build | flash | monitor | clean | erase-flash (默认 flash)
- `port`: 串口号, 如 COM11 (默认 COM11)
- `project_dir`: 项目目录 (默认当前目录的 `esp32c2_idf/` 子目录)

## 执行步骤

1. 解析参数: 从 `$ARGUMENTS` 中提取 action (默认 flash)、port (默认 COM11)、project_dir (默认 `esp32c2_idf`)
2. 若 action 是 `erase-flash`, 执行:
   ```powershell
   $idfPath = "D:\OpenSiFli\xiaozhi-sf32\esp32\ble_gateway\esp-idf\esp-idf"
   $pythonExe = "C:\Users\29538\.espressif\python_env\idf5.5_py3.13_env\Scripts\python.exe"
   $idfPy = "$idfPath\tools\idf.py"
   $toolchainBin = "C:\Users\29538\.espressif\tools\riscv32-esp-elf\esp-14.2.0_20251107\riscv32-esp-elf\bin"
   $cmakeBin = "C:\Users\29538\.espressif\tools\cmake\3.30.2\bin"
   $ninjaBin = "C:\Users\29538\.espressif\tools\ninja\1.12.1"
   $env:IDF_PATH = $idfPath
   $env:PATH = "$toolchainBin;$cmakeBin;$ninjaBin;$env:PATH"
   $env:MSYSTEM = ""
   & $pythonExe $idfPy -p <port> erase-flash 2>&1
   ```
3. 若 action 是 `flash`, 先 `Remove-Item -Recurse -Force build -ErrorAction SilentlyContinue` 清理旧构建, 再执行:
   ```powershell
   # (同上环境变量设置)
   & $pythonExe $idfPy -p <port> flash 2>&1 | Select-String -Pattern "error|FAILED|Wrote|Done|Connecting|Chip is|Leaving"
   ```
4. 若 action 是 `build`, 执行 `& $pythonExe $idfPy build 2>&1`
5. 若 action 是 `monitor`, 执行 `& $pythonExe $idfPy -p <port> monitor 2>&1 | Select-String -Pattern "Found|SSID|RSSI|scan|WIFI_SCAN|error|panic" | Select-Object -First 30`
6. 若 action 是 `clean`, 执行 `& $pythonExe $idfPy fullclean 2>&1`
7. **注意**: 执行 flash/monitor 前需确认串口未被占用 (串口监视器已关闭)

## 注意事项

- 工作目录必须是 ESP-IDF 项目目录 (含 CMakeLists.txt)
- flash 操作前自动清理 build 目录避免缓存问题
- 编译错误会直接输出, 无需额外过滤
- monitor 输出过滤常见关键词, 只显示前 30 行有意义内容
