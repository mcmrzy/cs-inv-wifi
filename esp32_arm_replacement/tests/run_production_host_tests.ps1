$ErrorActionPreference = 'Stop'
$gcc = 'C:\Users\29538\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe'
$python = 'C:\Users\29538\.espressif\python_env\idf5.5_py3.13_env\Scripts\python.exe'
Push-Location (Split-Path -Parent $PSScriptRoot)
try {
 New-Item -ItemType Directory -Force tmp,tmp/ui_graphical | Out-Null
 & $python tests/factory_pairing_tool_test.py
 if($LASTEXITCODE -ne 0){throw 'pairing tool failed'}
 & $gcc -std=c11 -Wall -Wextra -Werror -Imain/onboarding -Icomponents/monocypher/src -Icomponents/monocypher/src/optional tests/cloud_binding_codec_test.c main/onboarding/cloud_binding_codec.c components/monocypher/src/monocypher.c components/monocypher/src/optional/monocypher-ed25519.c -o tmp/cloud_binding_codec_test.exe
 if($LASTEXITCODE -ne 0){throw 'binding codec compile failed'}
 & ./tmp/cloud_binding_codec_test.exe
 if($LASTEXITCODE -ne 0){throw 'binding codec failed'}
 & $gcc -std=c11 -Wall -Wextra -Werror -Imain/onboarding tests/cloud_binding_policy_test.c -o tmp/cloud_binding_policy_test.exe
 if($LASTEXITCODE -ne 0){throw 'binding policy compile failed'}
 & ./tmp/cloud_binding_policy_test.exe
 if($LASTEXITCODE -ne 0){throw 'binding policy failed'}
 & ./tests/run_onboarding_host_tests.ps1
 if($LASTEXITCODE -ne 0){throw 'onboarding failed'}
 & $python tests/prepare_onboarding_panel_test.py
 if($LASTEXITCODE -ne 0){throw 'panel onboarding extraction failed'}
 & $gcc -std=c11 -Wall -Wextra -Werror -Imain/direct -Imain/telemetry -Imain/onboarding -Imain/ota tests/onboarding_panel_test.c -o tmp/onboarding_panel_test.exe
 if($LASTEXITCODE -ne 0){throw 'panel onboarding compile failed'}
 & ./tmp/onboarding_panel_test.exe
 if($LASTEXITCODE -ne 0){throw 'panel onboarding integration failed'}
 & $python tests/tls_memory_test.py
 if($LASTEXITCODE -ne 0){throw 'TLS memory regression failed'}
 & $python tests/ota_remote_memory_test.py
 if($LASTEXITCODE -ne 0){throw 'remote OTA memory regression failed'}
 & $python tests/ota_https_memory_test.py
 if($LASTEXITCODE -ne 0){throw 'HTTPS OTA memory/resume regression failed'}
 & $python tests/ota_http_resume_test.py
 if($LASTEXITCODE -ne 0){throw 'HTTP resume range regression failed'}
 & $python tests/ble_ota_memory_lifecycle_test.py
 if($LASTEXITCODE -ne 0){throw 'BLE OTA memory lifecycle regression failed'}
 & $python tests/dsp_info_test.py
 if($LASTEXITCODE -ne 0){throw 'DSP info regression failed'}
 & $python tests/dsp_import_tool_test.py
 if($LASTEXITCODE -ne 0){throw 'DSP import tool regression failed'}
 & $python tests/dsp_peer_tx_race_test.py
 if($LASTEXITCODE -ne 0){throw 'DSP peer collision reproduction failed'}
 & $python tests/dsp_defaults_source_test.py
 if($LASTEXITCODE -ne 0){throw 'original ARM defaults comparison failed'}
 & $python tests/controller_command_test.py
 if($LASTEXITCODE -ne 0){throw 'controller command save/readback regression failed'}
 & $python tests/controller_config_publish_test.py
 if($LASTEXITCODE -ne 0){throw 'controller config JSON regression failed'}
 & $python tests/telemetry_config_lock_test.py
 if($LASTEXITCODE -ne 0){throw 'telemetry config mutex integration failed'}
 & $python tests/prepare_dsp_runtime_test.py
 if($LASTEXITCODE -ne 0){throw 'DSP runtime extraction failed'}
 & $python tests/prepare_display_sample_panel_test.py
 if($LASTEXITCODE -ne 0){throw 'display sample panel extraction failed'}
 & $python tests/dsp_ota_owner_test.py
 if($LASTEXITCODE -ne 0){throw 'DSP OTA owner integration failed'}
 & $python tests/prepare_wifi_reconnect_test.py
 if($LASTEXITCODE -ne 0){throw 'WiFi reconnect extraction failed'}
 & $python tests/prepare_uart_setup_test.py
 if($LASTEXITCODE -ne 0){throw 'UART setup extraction failed'}
 & $python tests/prepare_admin_ota_test.py
 if($LASTEXITCODE -ne 0){throw 'admin OTA extraction failed'}
 & $gcc -std=c11 -Wall -Wextra -Werror tests/admin_ota_test.c -o tmp/admin_ota_test.exe
 if($LASTEXITCODE -ne 0){throw 'admin OTA compile failed'}
 & ./tmp/admin_ota_test.exe
 if($LASTEXITCODE -ne 0){throw 'admin OTA exclusion failed'}
 & $python tests/prepare_telemetry_fresh_test.py
 if($LASTEXITCODE -ne 0){throw 'telemetry extraction failed'}
 & $gcc -std=c11 -Wall -Wextra -Werror -Imain/telemetry tests/telemetry_fresh_test.c -o tmp/telemetry_fresh_test.exe
 if($LASTEXITCODE -ne 0){throw 'telemetry freshness compile failed'}
 & ./tmp/telemetry_fresh_test.exe
 if($LASTEXITCODE -ne 0){throw 'telemetry freshness failed'}
 & $python tests/prepare_serial_admin_test.py
 if($LASTEXITCODE -ne 0){throw 'serial extraction failed'}
 & $gcc -std=c11 -Wall -Wextra -Werror -Imain/app -Imain/factory tests/serial_admin_test.c main/app/admin_sn_command.c main/factory/factory_wire.c -o tmp/serial_admin_test.exe
 if($LASTEXITCODE -ne 0){throw 'serial command compile failed'}
 & ./tmp/serial_admin_test.exe
 if($LASTEXITCODE -ne 0){throw 'serial command failed'}
 & $python tests/prepare_factory_diagnostic_test.py
 if($LASTEXITCODE -ne 0){throw 'diagnostic extraction failed'}
 foreach($writes in @(0,1)){
  & $gcc -std=c11 -Wall -Wextra -Werror "-DCONFIG_DIRECT_FACTORY_WRITES=$writes" tests/factory_diagnostic_test.c -o tmp/factory_diagnostic_test.exe
  if($LASTEXITCODE -ne 0){throw 'diagnostic compile failed'}
  & ./tmp/factory_diagnostic_test.exe
  if($LASTEXITCODE -ne 0){throw 'diagnostic permission regression failed'}
 }
 & $python tests/factory_gui_backend_test.py
 if($LASTEXITCODE -ne 0){throw 'GUI protocol regression failed'}
 & $python tests/prepare_bms_poll_test.py
 if($LASTEXITCODE -ne 0){throw 'BMS poll extraction failed'}
 & $python tests/prepare_board_can_idle_test.py
 if($LASTEXITCODE -ne 0){throw 'CAN idle extraction failed'}
 $common = @('main/direct/direct_ui.c','main/direct/direct_gfx.c','main/direct/direct_gfx_assets.c','main/direct/direct_ui_assets.c','main/direct/direct_ui_locale.c')
 $cases = @(
 @{name='factory_store';src=@('main/factory/factory_record.c','main/factory/factory_store.c')},
 @{name='factory_identity';src=@('main/factory/factory_identity.c','main/factory/factory_record.c','main/factory/factory_store.c','main/factory/factory_pairing_record.c')},
 @{name='factory_pairing_identity';src=@('main/factory/factory_identity.c','main/factory/factory_record.c','main/factory/factory_store.c','main/factory/factory_pairing_record.c')},
 @{name='factory_pairing_record';src=@('main/factory/factory_pairing_record.c')},
 @{name='ota_compat';src=@('main/ota/ota_compat.c')},
 @{name='ota_health';src=@('main/ota/ota_health.c')},
 @{name='ota_image_identity';src=@('main/ota/ota_image_identity.c')},
 @{name='factory_wire';src=@('main/factory/factory_wire.c')},
 @{name='direct_iap';src=@('main/direct/direct_iap.c','main/direct/direct_protocol.c')},
 @{name='dsp_ota_transport';src=@('main/direct/direct_dsp_ota_transport.c','main/direct/direct_protocol.c')},
 @{name='display_sample';src=@('main/direct/direct_display_sample.c')},
 @{name='display_stale';src=@('main/direct/direct_display_sample.c')},
 @{name='display_event_regression';src=@('main/direct/direct_display_sample.c')},
 @{name='display_events';src=$common+@('main/direct/direct_display_sample.c','main/direct/direct_display_events.c')},
 @{name='display_sample_panel';src=@('main/direct/direct_display_sample.c')},
 @{name='protocol';src=@('main/direct/direct_protocol.c','main/direct/direct_lcd_codec.c')},
 @{name='dsp_codec';src=@('main/direct/direct_protocol.c')},
 @{name='dsp_stream';src=@('main/direct/direct_protocol.c','main/direct/direct_dsp_stream.c')},
 @{name='dsp_capture';src=@('main/direct/direct_protocol.c','main/direct/direct_dsp_stream.c')},
 @{name='dsp_params';src=@('main/direct/direct_protocol.c','main/direct/direct_dsp_params.c')},
 @{name='dsp_params_store';src=@('main/direct/direct_protocol.c','main/direct/direct_dsp_params.c','main/direct/direct_dsp_params_store.c')},
 @{name='dsp_runtime';src=@('main/direct/direct_protocol.c','main/direct/direct_dsp_stream.c','main/direct/direct_dsp_params.c')},
 @{name='wifi_reconnect';src=@()},
  @{name='uart_setup';src=@()},
 @{name='bms_identity';src=@('main/direct/direct_bms_identity.c','main/direct/direct_protocol.c')},
 @{name='bms_calibration';src=@('main/direct/direct_bms_calibration.c','main/direct/direct_protocol.c')},
 @{name='bms_poll_timing';src=@('main/direct/direct_protocol.c')},
 @{name='key_codec';src=@('main/direct/direct_key_codec.c')},
 @{name='energy';src=@('main/direct/direct_energy_codec.c')},
 @{name='lcd_transport';src=@('main/direct/direct_lcd_codec.c')},
 @{name='board_can_idle';src=@()},
 @{name='ui_upgrade';src=$common},
 @{name='ui_v050';src=$common},
 @{name='ui_keys_layout';src=$common+@('main/direct/direct_key_codec.c')},
 @{name='ui_locale';src=$common},
 @{name='ui_render';src=$common+@('main/direct/direct_ui_scene.c')}
 )
 foreach($case in $cases){
  $exe="tmp/$($case.name)_test.exe"
  & $gcc -std=c11 -Wall -Wextra -Werror -Imain/direct -Imain/telemetry -Imain/ota -Imain/factory -Itests/host_factory "tests/$($case.name)_test.c" @($case.src) -o $exe
  if($LASTEXITCODE -ne 0){throw "$($case.name) compile failed"}
  & $exe
  if($LASTEXITCODE -ne 0){throw "$($case.name) failed"}
 }
 & $python tests/factory_tool_test.py
 if($LASTEXITCODE -ne 0){throw 'tool test failed'}
 & $python tests/flash_production_test.py
 if($LASTEXITCODE -ne 0){throw 'flashing safeguards failed'}
 & $python tests/prepare_boot_sequence_test.py
 & $python tests/prepare_ota_boot_test.py
 if($LASTEXITCODE -ne 0){throw 'boot-health extraction failed'}
 & $python tests/prepare_direct_ota_test.py
 if($LASTEXITCODE -ne 0){throw 'direct OTA extraction failed'}
 & $gcc -std=c11 -Wall -Wextra -Werror -Imain/ota -Imain/direct tests/direct_ota_flow_test.c -o tmp/direct_ota_flow_test.exe
 if($LASTEXITCODE -ne 0){throw 'direct OTA flow compile failed'}
 & ./tmp/direct_ota_flow_test.exe
 if($LASTEXITCODE -ne 0){throw 'direct OTA flow failed'}
 & $python tests/prepare_iap_uart_test.py
 if($LASTEXITCODE -ne 0){throw 'UART sender extraction failed'}
 & $gcc -std=c11 -Wall -Wextra -Werror -Imain/direct tests/iap_uart_flow_test.c -o tmp/iap_uart_flow_test.exe
 if($LASTEXITCODE -ne 0){throw 'UART sender compile failed'}
 & ./tmp/iap_uart_flow_test.exe
 if($LASTEXITCODE -ne 0){throw 'UART sender failed'}
 foreach($production in @(0,1)){
  & $gcc -std=c11 -Wall -Wextra -Werror -Imain/ota "-DCONFIG_DIRECT_IDENTITY_PRODUCTION=$production" tests/ota_boot_flow_test.c main/ota/ota_health.c -o tmp/ota_boot_flow_test.exe
  if($LASTEXITCODE -ne 0){throw 'OTA boot flow compile failed'}
  & ./tmp/ota_boot_flow_test.exe
  if($LASTEXITCODE -ne 0){throw 'OTA boot flow failed'}
 }
 & $gcc -std=c11 -Wall -Wextra -Werror -Itests/host_splash -Imain/direct '-DFIRMWARE_VERSION="0.6.0-production-dev"' tests/boot_sequence_test.c main/direct/direct_splash.c main/direct/direct_boot_logo.c -o tmp/boot_sequence_test.exe
 if($LASTEXITCODE -ne 0){throw 'boot compile failed'}
 & ./tmp/boot_sequence_test.exe
 if($LASTEXITCODE -ne 0){throw 'boot failed'}
} finally {Pop-Location}

