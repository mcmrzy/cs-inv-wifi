param([string]$OutputName = 'lcd_isolation_20mhz_20261023')
$ErrorActionPreference = 'Stop'
if ($OutputName -notmatch '^[a-zA-Z0-9_-]+$') { throw 'OutputName must be a simple directory name' }
$projectRoot = Split-Path -Parent $PSScriptRoot
$output = Join-Path $projectRoot "tmp\$OutputName"
if (Test-Path -LiteralPath $output) { throw "Refusing to overwrite existing build: $output" }
$source = Join-Path $output 'source'
New-Item -ItemType Directory -Path $source -Force | Out-Null
# Snapshot the current worktree, including its existing fixes, without changing
# the normal version/config/build or copying credentials and release packages.
foreach ($dir in @('main', 'components')) {
    Copy-Item -LiteralPath (Join-Path $projectRoot $dir) -Destination $source -Recurse
}
foreach ($file in @('CMakeLists.txt', 'partitions.csv', 'sdkconfig.defaults')) {
    Copy-Item -LiteralPath (Join-Path $projectRoot $file) -Destination $source
}
[IO.File]::WriteAllText((Join-Path $source 'version.txt'), '0.6.43-lcd-isolate20', [Text.UTF8Encoding]::new($false))
$config = Get-Content -LiteralPath (Join-Path $projectRoot 'sdkconfig') -Raw
$overrides = [ordered]@{
    DIRECT_LCD_ISOLATION_TEST = 'y'
    DIRECT_LCD_SPI_CLOCK_HZ = '20000000'
    DIRECT_LCD_4WIRE = 'y'
    DIRECT_LCD_3WIRE = 'n'
    DIRECT_LCD_GPIO_SPI = 'n'
    DIRECT_LCD_READBACK_TERMINAL = 'n'
    DIRECT_LCD_COLOR_TEST = 'n'
    DIRECT_BOOT_DIAGNOSTICS = 'n'
    DIRECT_FACTORY_WRITES = 'n'
    DIRECT_IDENTITY_PRODUCTION = 'n'
}
foreach ($key in $overrides.Keys) {
    $config = $config -replace "(?m)^(?:CONFIG_${key}=.*|# CONFIG_${key} is not set)\r?\n?", ''
    $value = $overrides[$key]
    $config += if ($value -eq 'n') { "`n# CONFIG_$key is not set`n" } else { "`nCONFIG_$key=$value`n" }
}
[IO.File]::WriteAllText((Join-Path $source 'sdkconfig'), $config, [Text.UTF8Encoding]::new($false))
$idfRoot = 'D:\.espressif\v5.5.4\esp-idf'
$idfPython = 'C:\Users\29538\.espressif\python_env\idf5.5_py3.13_env\Scripts\python.exe'
$idfTools = 'C:\Users\29538\.espressif\tools'
$env:IDF_PATH = $idfRoot
$env:IDF_TOOLS_PATH = 'C:\Users\29538\.espressif'
$env:IDF_PYTHON_ENV_PATH = Split-Path -Parent (Split-Path -Parent $idfPython)
$env:ESP_ROM_ELF_DIR = "$idfTools\esp-rom-elfs\20241011\"
$env:Path = "$idfTools\cmake\3.30.2\bin;$idfTools\ninja\1.12.1;$idfTools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin;$idfTools\idf-exe\1.0.3;$idfTools\ccache\4.11.2;$env:Path"
Push-Location $source
try {
    & $idfPython "$idfRoot\tools\idf.py" -B (Join-Path $output 'build') build *> (Join-Path $output 'build.log')
    if ($LASTEXITCODE -ne 0) { throw "Build failed; see $output\build.log" }
    $symbols = & "$idfTools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin\riscv32-esp-elf-nm.exe" --defined-only (Join-Path $output 'build\cs_inv_direct_c3.elf')
    if ($LASTEXITCODE -ne 0) { throw 'ELF symbol inspection failed' }
    $symbols | Set-Content -LiteralPath (Join-Path $output 'symbols.txt')
    $forbidden = '\b(esp_wifi_init|esp_wifi_start|esp_bt_controller_init|nimble_port_init|esp_mqtt_client_init|wifi_sta_init|ble_ct_init|mqtt_handler_init|panel_task|direct_keys_init|arm_poll_init|storage_init)$'
    if ($symbols -match $forbidden) { throw 'Isolation ELF unexpectedly retains a wireless or business initializer' }
    & $idfPython (Join-Path $projectRoot 'tools\firmware_budget.py') --build-dir (Join-Path $output 'build') --output (Join-Path $output 'budget.json') *> (Join-Path $output 'budget.log')
    if ($LASTEXITCODE -ne 0) { throw 'Firmware budget verification failed' }
    Copy-Item -LiteralPath (Join-Path $output 'build\cs_inv_direct_c3.bin') -Destination (Join-Path $output 'lcd_isolation_20mhz_app.bin')
    Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $output 'lcd_isolation_20mhz_app.bin') |
        Format-List | Out-String | Set-Content -LiteralPath (Join-Path $output 'SHA256.txt')
    Write-Output "App image: $output\lcd_isolation_20mhz_app.bin"
} finally { Pop-Location }
