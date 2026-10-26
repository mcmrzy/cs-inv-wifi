param(
 [string]$Gcc = 'C:\Users\29538\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe',
 [string]$Python = 'C:\Users\29538\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'
)
$ErrorActionPreference = 'Stop'
Push-Location (Split-Path -Parent $PSScriptRoot)
try {
 New-Item -ItemType Directory -Force tmp,tmp/ui_graphical | Out-Null
 & $Python tests/prepare_display_sample_panel_test.py
 if($LASTEXITCODE -ne 0){throw 'display sample panel extraction failed'}
 $common=@('main/direct/direct_ui.c','main/direct/direct_gfx.c','main/direct/direct_gfx_assets.c','main/direct/direct_ui_assets.c','main/direct/direct_ui_locale.c','main/direct/direct_battery_config.c')
 $homeSources=@('main/direct/direct_home.c','main/direct/direct_home_asset.c','main/direct/direct_home_font.c')
 $cases=@(
  @{name='home_model';src=@('main/direct/direct_home.c')},
  @{name='tn_settings';src=$common},
  @{name='ui';src=$common},
  @{name='home_asset';src=@('main/direct/direct_home_asset.c','main/direct/direct_gfx.c','main/direct/direct_gfx_assets.c')},
  @{name='home_font';src=@('main/direct/direct_home_font.c','main/direct/direct_gfx.c','main/direct/direct_gfx_assets.c')},
  @{name='display_events';src=$common+@('main/direct/direct_display_sample.c','main/direct/direct_display_events.c','main/direct/direct_home.c')},
  @{name='display_stale';src=@('main/direct/direct_display_sample.c')},
  @{name='display_event_regression';src=@('main/direct/direct_display_sample.c')},
  @{name='display_sample_panel';src=@('main/direct/direct_display_sample.c')},
  @{name='ui_keys_layout';src=$common+@('main/direct/direct_key_codec.c')},
  @{name='ui_v050';src=$common},
  @{name='ui_upgrade';src=$common},
  @{name='ui_locale';src=$common},
  @{name='ui_render';src=$common+$homeSources+@('main/direct/direct_ui_scene.c')}
 )
 foreach($case in $cases){
  $exe="tmp/$($case.name)_test.exe"
  & $Gcc -save-temps=obj -std=c11 -Wall -Wextra -Werror -Imain/direct -Imain/telemetry -Imain/ota "tests/$($case.name)_test.c" @($case.src) -o $exe
  if($LASTEXITCODE -ne 0){throw "$($case.name) compile failed"}
  & $exe
  if($LASTEXITCODE -ne 0){throw "$($case.name) failed"}
 }
} finally {Pop-Location}
