# build_test.ps1 — 宿主编译并运行 cs_protocol 交叉验证测试
# 用法: powershell -ExecutionPolicy Bypass -File build_test.ps1
$ErrorActionPreference = "Stop"

$msvc = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231"
$cl = "$msvc\bin\Hostx64\x64\cl.exe"
$inc = "$msvc\include"
$lib = "$msvc\lib\x64"
$kits = "C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0"
$kitslib = "C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0"

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$proj = "$root\esp32c3_l10_idf\main\telemetry"
$testdir = "$root\tools\arm_simulator_l10"
$out = Join-Path $env:TEMP "test_cs_protocol.exe"

& $cl /nologo /W3 /utf-8 /I"$proj" /I"$testdir" /I"$inc" /I"$kits\ucrt" /I"$kits\shared" /I"$kits\um" `
    "$testdir\test_cs_protocol.c" "$proj\cs_protocol.c" /Fe:"$out" `
    /link /LIBPATH:"$lib" /LIBPATH:"$kitslib\ucrt\x64" /LIBPATH:"$kitslib\um\x64"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $out
exit $LASTEXITCODE
