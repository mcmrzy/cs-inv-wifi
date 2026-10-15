$ErrorActionPreference = "Continue"
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
Remove-Item Env:MINGW_PREFIX -ErrorAction SilentlyContinue
$env:IDF_PYTHON_ENV_PATH = "C:\Users\29538\.espressif\python_env\idf5.5_py3.13_env"
$PY = "$env:IDF_PYTHON_ENV_PATH\Scripts\python.exe"
Set-Location "d:\CS_INV_WIFI\esp32c3_l10_idf"
& $PY -m esptool --chip esp32c3 -p COM7 -b 460800 --before default_reset --after hard_reset write_flash 0x20000 build\cs_inv_l10_c3.bin
Write-Output "FLASH_EXIT=$LASTEXITCODE"
