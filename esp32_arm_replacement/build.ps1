param(
    [ValidateSet('Device','Factory','Production')]
    [string]$Profile = 'Device',
    [switch]$Diagnostics
)
$ErrorActionPreference = 'Stop'

$idfRoot = 'D:\.espressif\v5.5.4\esp-idf'
$idfPython = 'C:\Users\29538\.espressif\python_env\idf5.5_py3.13_env\Scripts\python.exe'
$idfTools = 'C:\Users\29538\.espressif\tools'

if (-not (Test-Path -LiteralPath $idfPython -PathType Leaf)) {
    throw "ESP-IDF Python missing: $idfPython"
}
if (-not (Test-Path -LiteralPath (Join-Path $idfRoot 'tools\idf.py') -PathType Leaf)) {
    throw "ESP-IDF source missing: $idfRoot"
}

$env:IDF_PATH = $idfRoot
$env:IDF_TOOLS_PATH = 'C:\Users\29538\.espressif'
$env:IDF_PYTHON_ENV_PATH = Split-Path -Parent (Split-Path -Parent $idfPython)
$env:ESP_ROM_ELF_DIR = "$idfTools\esp-rom-elfs\20241011\"
$env:Path = "$idfTools\cmake\3.30.2\bin;$idfTools\ninja\1.12.1;$idfTools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin;$idfTools\idf-exe\1.0.3;$idfTools\ccache\4.11.2;$env:Path"

Push-Location $PSScriptRoot
try {
    $profileConfig = Join-Path $PSScriptRoot "sdkconfig.$($Profile.ToLower())"
    $baseConfig = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'sdkconfig') -Raw
    $baseConfig = $baseConfig -replace '(?m)^#?\s*CONFIG_DIRECT_IDENTITY_PRODUCTION.*\r?\n',''
    $baseConfig = $baseConfig -replace '(?m)^#?\s*CONFIG_DIRECT_FACTORY_WRITES.*\r?\n',''
    $baseConfig = $baseConfig -replace '(?m)^#?\s*CONFIG_DIRECT_BOOT_DIAGNOSTICS.*\r?\n',''
    $baseConfig += if($Diagnostics){"`nCONFIG_DIRECT_BOOT_DIAGNOSTICS=y`n"}else{"`n# CONFIG_DIRECT_BOOT_DIAGNOSTICS is not set`n"}
    $baseConfig = $baseConfig -replace 'CONFIG_BOOTLOADER_LOG_LEVEL_INFO=y','# CONFIG_BOOTLOADER_LOG_LEVEL_INFO is not set'
    $baseConfig = $baseConfig -replace '# CONFIG_BOOTLOADER_LOG_LEVEL_WARN is not set','CONFIG_BOOTLOADER_LOG_LEVEL_WARN=y'
    $baseConfig = $baseConfig -replace '(?m)^CONFIG_BOOTLOADER_LOG_LEVEL=3','CONFIG_BOOTLOADER_LOG_LEVEL=2'
    if ($Profile -eq 'Device') {
        $baseConfig += "`n# CONFIG_DIRECT_IDENTITY_PRODUCTION is not set`n# CONFIG_DIRECT_FACTORY_WRITES is not set`n"
    } elseif ($Profile -eq 'Factory') {
        $baseConfig += "`nCONFIG_DIRECT_IDENTITY_PRODUCTION=y`nCONFIG_DIRECT_FACTORY_WRITES=y`n"
    } else {
        $baseConfig += "`nCONFIG_DIRECT_IDENTITY_PRODUCTION=y`n# CONFIG_DIRECT_FACTORY_WRITES is not set`n"
    }
    [IO.File]::WriteAllText($profileConfig,$baseConfig,[Text.UTF8Encoding]::new($false))
    & $idfPython (Join-Path $idfRoot 'tools\idf.py') -D "SDKCONFIG=$profileConfig" build
    if ($LASTEXITCODE -ne 0) { throw "ESP-IDF build failed: exit $LASTEXITCODE" }
} finally {
    Pop-Location
}
