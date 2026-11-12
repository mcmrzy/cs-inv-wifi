param([string]$Gcc='C:/Users/29538/AppData/Local/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64/bin/gcc.exe')
$ErrorActionPreference='Stop'
Push-Location (Split-Path -Parent $PSScriptRoot)
try {
    New-Item -ItemType Directory -Force tmp | Out-Null
    & $Gcc -save-temps=obj -std=c11 -Wall -Wextra -Werror -Imain -I../esp32_modbus_panel/main -I../esp32_direct/main/direct tests/core_test.c main/diag_core.c ../esp32_modbus_panel/main/panel_modbus.c ../esp32_direct/main/direct/direct_protocol.c -o tmp/core_test.exe
    if($LASTEXITCODE -ne 0){throw 'core compile failed'}
    & ./tmp/core_test.exe
    if($LASTEXITCODE -ne 0){throw 'core test failed'}
    & $Gcc -save-temps=obj -std=c11 -Wall -Wextra -Werror -Imain -Itests -I../esp32_modbus_panel/main tests/runner_test.c main/diag_core.c ../esp32_modbus_panel/main/panel_modbus.c -o tmp/runner_test.exe
    if($LASTEXITCODE -ne 0){throw 'runner compile failed'}
    & ./tmp/runner_test.exe
    if($LASTEXITCODE -ne 0){throw 'runner test failed'}
} finally {Pop-Location}
