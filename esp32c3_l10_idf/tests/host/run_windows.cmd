@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
set "PATH=C:\Users\29538\.espressif\tools\cmake\3.30.2\bin;C:\Users\29538\.espressif\tools\ninja\1.12.1;%PATH%"
cd /d "%~dp0..\.."
cmake -S tests/host -B build/ble-host-tests -G Ninja -DCJSON_DIR=D:/.espressif/v5.5.4/esp-idf/components/json/cJSON -DCMAKE_MAKE_PROGRAM=C:/Users/29538/.espressif/tools/ninja/1.12.1/ninja.exe
if errorlevel 1 exit /b 1
cmake --build build/ble-host-tests
if errorlevel 1 exit /b 1
ctest --test-dir build/ble-host-tests --output-on-failure %*
exit /b %errorlevel%
