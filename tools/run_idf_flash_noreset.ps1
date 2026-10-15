# ESP32-C3 烧录 (no_reset 模式, 需要设备已处于下载模式)
# 用法: 若自动复位失败, 请先按住 BOOT 键再按 RST(或重新上电时按住 BOOT), 然后运行本脚本
$ErrorActionPreference = "Continue"
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
Remove-Item Env:MINGW_PREFIX -ErrorAction SilentlyContinue
$env:IDF_PATH = "D:\.espressif\v5.5.4\esp-idf"
$env:IDF_TOOLS_PATH = "C:\Users\29538\.espressif"
$env:IDF_PYTHON_ENV_PATH = "C:\Users\29538\.espressif\python_env\idf5.5_py3.13_env"
$PY = "$env:IDF_PYTHON_ENV_PATH\Scripts\python.exe"
$T = "C:\Users\29538\.espressif\tools"
$env:PATH = "$T\cmake\3.30.2\bin;$T\ninja\1.12.1;$T\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin;$T\idf-exe\1.0.3;$T\ccache\4.11.2;$T\dfu-util\0.11;$env:PATH"
Set-Location "d:\CS_INV_WIFI\esp32c3_l10_idf\build"
& $PY "$env:IDF_PATH\components\esptool_py\esptool\esptool.py" --chip esp32c3 -p COM3 -b 115200 --before no_reset --after hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB 0x0 bootloader/bootloader.bin 0x20000 cs_inv_l10_c3.bin 0x8000 partition_table/partition-table.bin 0x10000 ota_data_initial.bin 2>&1 | Tee-Object -FilePath "d:\CS_INV_WIFI\tools\idf_flash_nr.log"
Write-Output "FLASH_EXIT=$LASTEXITCODE"
exit $LASTEXITCODE
