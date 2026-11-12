param([string]$Gcc='C:\Users\29538\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe')
$ErrorActionPreference='Stop'
Push-Location (Split-Path -Parent $PSScriptRoot)
try {
    New-Item -ItemType Directory -Force tmp | Out-Null
    $shared='../esp32_direct/main/direct'
    $cases=@(
        @{name='bms_diagnostics';src=@('full/bms_diagnostics.c','full/bms_bbj_frame.c',"$shared/direct_protocol.c")},
        @{name='key_layout';src=@("$shared/direct_key_codec.c");defines=@('-DPANEL_FULL_DIRECT=1')},
        @{name='legacy_key_codec';test='../esp32_direct/tests/key_codec_test.c';src=@("$shared/direct_key_codec.c")},
        @{name='model';src=@('main/panel_model.c')},
        @{name='home_model';test='../esp32_direct/tests/home_model_test.c';src=@("$shared/direct_home.c")},
        @{name='alarm';src=@('main/panel_alarm.c','main/panel_modbus.c')},
        @{name='alarm_service';src=@('main/panel_alarm.c','main/panel_modbus.c')},
        @{name='indicators';src=@('main/panel_indicators.c','main/panel_model.c')},
        @{name='indicators_hw';src=@('main/panel_indicators.c','main/panel_model.c')},
        @{name='display_settings';src=@('main/panel_display_settings.c')},
        @{name='core';src=@('main/panel_modbus.c','main/panel_catalog.c','main/panel_model.c')},
        @{name='control';src=@('main/panel_control.c','main/panel_catalog.c','main/panel_model.c')},
        @{name='settings_query';src=@('main/panel_settings_query.c','main/panel_catalog.c','main/panel_model.c')},
        @{name='panel_wifi';src=@()},
        @{name='panel_connected_ui';src=@()},
        @{name='ui';src=@('main/panel_ui.c','main/panel_locale.c','main/panel_catalog.c','main/panel_model.c',"$shared/direct_gfx.c",'main/panel_fonts.c')},
        @{name='screen';src=@('main/panel_ui.c','main/panel_locale.c','main/panel_catalog.c','main/panel_model.c',"$shared/direct_gfx.c",'main/panel_fonts.c')},
        @{name='user_panel';src=@('main/panel_ui.c','main/panel_locale.c','main/panel_catalog.c','main/panel_model.c',"$shared/direct_gfx.c",'main/panel_fonts.c')},
        @{name='product_flow';src=@('main/panel_ui.c','main/panel_locale.c','main/panel_catalog.c','main/panel_model.c',"$shared/direct_gfx.c",'main/panel_fonts.c')},
        @{name='native_state';src=@('main/panel_ui.c','main/panel_locale.c','main/panel_catalog.c','main/panel_model.c',"$shared/direct_gfx.c",'main/panel_fonts.c')},
        @{name='full_mapping';src=@('full/full_modbus_map.c','main/panel_catalog.c','main/panel_model.c')},
        @{name='full_routes';src=@('full/direct_ui.c','main/panel_display_settings.c','full/ota_image_identity.c','full/direct_ui_locale.c',"$shared/direct_gfx.c",'main/panel_fonts.c')}
    )
    $renderSources=@('full/direct_ui.c','main/panel_display_settings.c',"$shared/direct_gfx.c",'main/panel_fonts.c','full/direct_ui_locale.c',"$shared/direct_home.c","$shared/direct_home_asset.c","$shared/direct_home_font.c",'full/direct_ui_scene.c')
    $cases+=@{name='full_render';test='tests/full_local_ui_test.c';src=$renderSources}
    New-Item -ItemType Directory -Force tmp/ui_graphical | Out-Null
    foreach($case in $cases) {
        $exe="tmp/$($case.name)_test.exe"
        $test=if($case.test){$case.test}else{"tests/$($case.name)_test.c"}
        & $Gcc -save-temps=obj -std=c11 -Wall -Wextra -Werror -DPANEL_USE_REAL_MODEL @($case.defines) -Imain -Ifull -Itests "-I$shared" -I../esp32_direct/main/telemetry -I../esp32_direct/main/ota $test @($case.src) -o $exe
        if($LASTEXITCODE -ne 0){throw "$($case.name) compile failed"}
        & ".\$exe"
        if($LASTEXITCODE -ne 0){throw "$($case.name) failed"}
    }
    & 'C:/Users/29538/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe' -X utf8 tests/preferences_test.py $Gcc
    if($LASTEXITCODE -ne 0){throw 'preferences regression failed'}
    & 'C:/Users/29538/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe' -X utf8 tests/buzzer_runtime_test.py $Gcc
    if($LASTEXITCODE -ne 0){throw 'passive buzzer regression failed'}
    & 'C:/Users/29538/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe' -X utf8 tests/font_codec_test.py $Gcc
    if($LASTEXITCODE -ne 0){throw 'font codec regression failed'}
    & 'C:/Users/29538/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe' -X utf8 tests/native_locale_test.py $Gcc
    if($LASTEXITCODE -ne 0){throw 'native locale regression failed'}
    & 'C:/Users/29538/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe' -X utf8 tests/full_poll_source_test.py
    if($LASTEXITCODE -ne 0){throw 'full poll source regression failed'}
    & 'C:/Users/29538/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe' -X utf8 tests/uart_board_test.py $Gcc
    if($LASTEXITCODE -ne 0){throw 'UART board regression failed'}
    & 'C:/Users/29538/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe' -X utf8 tests/display_runtime_test.py $Gcc
    if($LASTEXITCODE -ne 0){throw 'display runtime regression failed'}
    & 'C:/Users/29538/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe' -X utf8 tests/lvgl_boot_test.py $Gcc
    if($LASTEXITCODE -ne 0){throw 'LVGL boot sequence regression failed'}
    & 'C:/Users/29538/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe' -X utf8 tests/native_runtime_test.py $Gcc
    if($LASTEXITCODE -ne 0){throw 'native runtime regression failed'}
    & 'C:/Users/29538/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe' -X utf8 tests/english_layout_test.py $Gcc
    if($LASTEXITCODE -ne 0){throw 'English layout regression failed'}
    & 'C:/Users/29538/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe' -X utf8 tests/registry_info_test.py $Gcc
    if($LASTEXITCODE -ne 0){throw 'model registration info regression failed'}
    & 'C:/Users/29538/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe' -X utf8 tests/control_polling_test.py $Gcc
    if($LASTEXITCODE -ne 0){throw 'control polling regression failed'}
    foreach($script in @('bus_recovery_test.py','bms_recovery_test.py','bms_diagnostic_runtime_test.py','csergy_bms_test.py','bus_startup_test.py','time_transaction_test.py','wifi_profiles_test.py','json_number_test.py','pairing_test.py','firmware_catalog_test.py','panel_ota_tls_test.py','weather_test.py')) {
        & 'C:/Users/29538/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe' -X utf8 "tests/$script" $Gcc
        if($LASTEXITCODE -ne 0){throw "$script failed"}
    }
    foreach($script in @('legacy_protocol_test.py','ble_capability_test.py')) {
        & 'C:/Users/29538/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe' -X utf8 "tests/$script"
        if($LASTEXITCODE -ne 0){throw "$script failed"}
    }
    & 'C:/Users/29538/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe' -X utf8 tests/ble_admission_test.py $Gcc
    if($LASTEXITCODE -ne 0){throw 'BLE admission regression failed'}
    & 'C:/Users/29538/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe' -X utf8 tests/panel_ble_aux_test.py $Gcc
    if($LASTEXITCODE -ne 0){throw 'panel BLE telemetry regression failed'}
    & 'C:/Users/29538/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe' -X utf8 tests/alarm_transport_test.py $Gcc
    if($LASTEXITCODE -ne 0){throw 'alarm transport regression failed'}
    & 'C:/Users/29538/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe' -X utf8 tests/alarm_cloud_contract_test.py
    if($LASTEXITCODE -ne 0){throw 'alarm cloud contract failed'}
    $env:HOST_GCC=$Gcc
    foreach($script in @('offline_cache_codec_test.py','offline_cache_memory_test.py','offline_heartbeat_replay_test.py','telemetry_bms_test.py')) {
        & 'C:/Users/29538/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe' -X utf8 "../esp32_direct/tests/$script"
        if($LASTEXITCODE -ne 0){throw "$script failed"}
    }
} finally { Pop-Location }



