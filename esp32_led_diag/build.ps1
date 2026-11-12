param([string]$IdfRoot='D:/.espressif/v5.5.4/esp-idf',
      [string]$IdfPython='C:/Users/29538/.espressif/python_env/idf5.5_py3.13_env/Scripts/python.exe',
      [switch]$Rainbow)
$ErrorActionPreference='Stop'
$idfTools='C:\Users\29538\.espressif\tools'
$env:IDF_PATH=$IdfRoot
$env:IDF_TOOLS_PATH='C:\Users\29538\.espressif'
$env:IDF_PYTHON_ENV_PATH=Split-Path -Parent (Split-Path -Parent $IdfPython)
$env:ESP_ROM_ELF_DIR="$idfTools\esp-rom-elfs\20241011\"
$env:Path="$idfTools\cmake\3.30.2\bin;$idfTools\ninja\1.12.1;$idfTools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin;$idfTools\idf-exe\1.0.3;$env:Path"
Push-Location $PSScriptRoot
try {
    $rainbowOption=if($Rainbow){'ON'}else{'OFF'}
    & $IdfPython (Join-Path $IdfRoot 'tools/idf.py') -D "WS2812_RAINBOW=$rainbowOption" build
    if($LASTEXITCODE -ne 0){throw "Build failed: $LASTEXITCODE"}
} finally {Pop-Location}
