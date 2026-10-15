$ErrorActionPreference='Stop'
$gcc='C:\Users\29538\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe'
$python='C:\Users\29538\.espressif\python_env\idf5.5_py3.13_env\Scripts\python.exe'
Push-Location (Split-Path -Parent $PSScriptRoot)
try {
 $common=@('-std=c11','-Wall','-Wextra','-Werror','-Imain/onboarding','-Imain/direct','-Icomponents/qrcodegen')
 & $gcc @common tests/onboarding_state_test.c main/onboarding/onboarding_state.c -o tmp/onboarding_state_test.exe
 if($LASTEXITCODE -ne 0){throw 'state compile failed'}
 & ./tmp/onboarding_state_test.exe
 if($LASTEXITCODE -ne 0){throw 'state test failed'}
 $qr=@('main/onboarding/onboarding_qr.c','components/qrcodegen/qrcodegen.c')
 & $gcc @common tests/onboarding_qr_test.c @qr -o tmp/onboarding_qr_test.exe
 if($LASTEXITCODE -ne 0){throw 'QR compile failed'}
 & ./tmp/onboarding_qr_test.exe tmp/onboarding_qr.pgm
 if($LASTEXITCODE -ne 0){throw 'QR test failed'}
 & $gcc @common tests/onboarding_scene_test.c main/direct/direct_onboarding_scene.c main/direct/direct_gfx.c main/direct/direct_gfx_assets.c @qr -o tmp/onboarding_scene_test.exe
 if($LASTEXITCODE -ne 0){throw 'scene compile failed'}
 & ./tmp/onboarding_scene_test.exe tmp/onboarding_scene
 if($LASTEXITCODE -ne 0){throw 'scene test failed'}
 & $python tests/onboarding_qr_decode_test.py
 if($LASTEXITCODE -ne 0){throw 'independent QR decode failed'}
} finally { Pop-Location }
