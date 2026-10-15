# ESP32-C3 采集器固件 IDF 交叉编译脚本 (ESP-IDF 5.5.4)
# 直接设置 IDF 环境变量, 绕过 export.ps1 交互卡死问题
$ErrorActionPreference = "Continue"
Write-Output "DBG MSYSTEM=$env:MSYSTEM"
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
Remove-Item Env:MINGW_PREFIX -ErrorAction SilentlyContinue
$env:IDF_PATH = "D:\.espressif\v5.5.4\esp-idf"
$env:IDF_TOOLS_PATH = "C:\Users\29538\.espressif"
$env:IDF_PYTHON_ENV_PATH = "C:\Users\29538\.espressif\python_env\idf5.5_py3.13_env"
$PY = "$env:IDF_PYTHON_ENV_PATH\Scripts\python.exe"
# 手动将 IDF 工具链加入 PATH (cmake/ninja/riscv32-esp-elf 等)
$T = "C:\Users\29538\.espressif\tools"
$env:PATH = "$T\cmake\3.30.2\bin;$T\ninja\1.12.1;$T\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin;$T\idf-exe\1.0.3;$T\ccache\4.11.2;$T\dfu-util\0.11;$env:PATH"
Set-Location "d:\CS_INV_WIFI\esp32c3_l10_idf"
# 工具链版本已变更, 清理旧构建缓存
if (Test-Path build) { Remove-Item -Recurse -Force build }
if (Test-Path sdkconfig) { Remove-Item -Force sdkconfig }
& $PY "$env:IDF_PATH\tools\idf.py" build 2>&1 | Tee-Object -FilePath "d:\CS_INV_WIFI\tools\idf_build.log"
Write-Output "BUILD_EXIT=$LASTEXITCODE"
exit $LASTEXITCODE
