# ESP32-C3 采集器固件烧录脚本 (ESP-IDF 5.5.4) -> COM3
# 复用 run_idf_build.ps1 的环境设置, 直接烧录已构建产物
$ErrorActionPreference = "Continue"
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
Remove-Item Env:MINGW_PREFIX -ErrorAction SilentlyContinue
$env:IDF_PATH = "D:\.espressif\v5.5.4\esp-idf"
$env:IDF_TOOLS_PATH = "C:\Users\29538\.espressif"
$env:IDF_PYTHON_ENV_PATH = "C:\Users\29538\.espressif\python_env\idf5.5_py3.13_env"
$PY = "$env:IDF_PYTHON_ENV_PATH\Scripts\python.exe"
$T = "C:\Users\29538\.espressif\tools"
$env:PATH = "$T\cmake\3.30.2\bin;$T\ninja\1.12.1;$T\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin;$T\idf-exe\1.0.3;$T\ccache\4.11.2;$T\dfu-util\0.11;$env:PATH"
Set-Location "d:\CS_INV_WIFI\esp32c3_l10_idf"
# 构建产物已最新, flash 自动跳过重编译
# -b 115200: 460800 下 stub 切波特率后 flash 通信不稳 (flash_id 默认 115200 成功)
& $PY "$env:IDF_PATH\tools\idf.py" -p COM3 -b 115200 flash 2>&1 | Tee-Object -FilePath "d:\CS_INV_WIFI\tools\idf_flash.log"
Write-Output "FLASH_EXIT=$LASTEXITCODE"
exit $LASTEXITCODE
