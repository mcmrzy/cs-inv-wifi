param(
    [string]$IdfRoot = 'D:\.espressif\v5.5.4\esp-idf',
    [string]$IdfPython = 'C:\Users\29538\.espressif\python_env\idf5.5_py3.13_env\Scripts\python.exe',
    [switch]$BmsDiagnostic
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $IdfPython)) { throw "ESP-IDF Python missing: $IdfPython" }
$idfTools = 'C:\Users\29538\.espressif\tools'
$env:IDF_PATH = $IdfRoot
$env:IDF_TOOLS_PATH = 'C:\Users\29538\.espressif'
$env:IDF_PYTHON_ENV_PATH = Split-Path -Parent (Split-Path -Parent $IdfPython)
$env:ESP_ROM_ELF_DIR = "$idfTools\esp-rom-elfs\20241011\"
$env:Path = "$idfTools\cmake\3.30.2\bin;$idfTools\ninja\1.12.1;$idfTools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin;$idfTools\idf-exe\1.0.3;$idfTools\ccache\4.11.2;$env:Path"
Push-Location $PSScriptRoot
try {
    if($BmsDiagnostic){
        if(-not (Test-Path -LiteralPath sdkconfig.bms_diag)){
            Copy-Item -LiteralPath sdkconfig.full -Destination sdkconfig.bms_diag
        }
        # Defaults do not override an existing sdkconfig. Apply the dedicated
        # diagnostic overlay on every build, leaving sdkconfig.full untouched.
        $diagConfig = Get-Content -LiteralPath sdkconfig.bms_diag
        foreach($line in Get-Content -LiteralPath full/sdkconfig.bms_diag.defaults){
            if($line -match '^(CONFIG_[A-Z0-9_]+)='){
                $setting = $Matches[1]
                $diagConfig = @($diagConfig | Where-Object {$_ -notmatch "^$setting=" -and $_ -ne "# $setting is not set"})
                $diagConfig += $line
            }
        }
        [System.IO.File]::WriteAllLines((Join-Path $PSScriptRoot 'sdkconfig.bms_diag'),$diagConfig,[System.Text.UTF8Encoding]::new($false))
        & $IdfPython (Join-Path $IdfRoot 'tools\idf.py') -B build_bms_diag -D SDKCONFIG=sdkconfig.bms_diag -D PANEL_BMS_DIAGNOSTIC=ON build
    }else{
        & $IdfPython (Join-Path $IdfRoot 'tools\idf.py') -B build_full -D SDKCONFIG=sdkconfig.full -D PANEL_BMS_DIAGNOSTIC=OFF build
    }
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $LASTEXITCODE" }
} finally { Pop-Location }
