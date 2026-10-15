param(
 [string]$FirmwareRelease = 'release/esp32c3_v0.6.7_onboarding_dev_20261013',
 [switch]$SkipExeBuild,
 [switch]$SkipFirmwareCopy
)
$ErrorActionPreference = 'Stop'
$taskPython = 'C:\Users\29538\.espressif\python_env\idf5.5_py3.13_env\Scripts\python.exe'
$taskRoot = Split-Path -Parent $PSScriptRoot
Push-Location $taskRoot
try {
 $taskVersionValue = & $taskPython -c "import sys;sys.path.insert(0,'tools');import factory_gui_backend;print(factory_gui_backend.VERSION)"
 if ($LASTEXITCODE -ne 0) { throw 'Version discovery failed' }
 $taskOut = Join-Path $taskRoot "release\CSERGY_FactoryTool_v$taskVersionValue"
 $taskVersion = Join-Path $PSScriptRoot 'factory_gui_version.txt'
 if (-not $SkipExeBuild) {
  & $taskPython -m PyInstaller --noconfirm --clean --onefile --windowed --name CSERGY_FactoryTool --icon NONE --version-file $taskVersion --paths tools --collect-all customtkinter --collect-all esptool --collect-submodules serial --distpath $taskOut --workpath tmp/gui_build --specpath tmp tools/factory_gui.py
  if ($LASTEXITCODE -ne 0) { throw 'EXE build failed' }
 }
 if (-not (Test-Path (Join-Path $taskOut 'CSERGY_FactoryTool.exe'))) { throw 'EXE missing' }
 if (-not $SkipFirmwareCopy) {
  $taskSource = [IO.Path]::GetFullPath((Join-Path $taskRoot $FirmwareRelease))
  foreach($taskProfile in @('device','factory','production')) {
   $taskProfileSource = Join-Path $taskSource $taskProfile
   if (-not (Test-Path (Join-Path $taskProfileSource 'flash_manifest.json'))) { throw "Firmware package not ready: $taskProfileSource" }
   $taskProfileDest = Join-Path $taskOut "firmware\$taskProfile"
   New-Item -ItemType Directory -Force $taskProfileDest | Out-Null
   Copy-Item -Path (Join-Path $taskProfileSource '*') -Destination $taskProfileDest -Recurse -Force
  }
 }
 Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'README_GUI.md') -Destination (Join-Path $taskOut '使用说明.md') -Force
 Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'factory_gui_profile.example.json') -Destination (Join-Path $taskOut '工单示例.json') -Force
 Write-Output "EXE ready: $taskOut\CSERGY_FactoryTool.exe"
} finally {Pop-Location}
