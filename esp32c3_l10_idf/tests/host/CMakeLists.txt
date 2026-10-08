cmake_minimum_required(VERSION 3.16)
project(ble_host_tests C)
enable_testing()
set(CJSON_DIR "$ENV{IDF_PATH}/components/json/cJSON" CACHE PATH "Directory containing the firmware's cJSON.c")
if(NOT EXISTS "${CJSON_DIR}/cJSON.c")
  message(FATAL_ERROR "Set CJSON_DIR to the ESP-IDF cJSON directory")
endif()
add_executable(test_ble_control test_ble_control.c
  ../../components/ble_ct/ble_ct_cmd.c
  "${CJSON_DIR}/cJSON.c")
target_include_directories(test_ble_control PRIVATE ble_stubs ../../components/ble_ct "${CJSON_DIR}")
target_compile_features(test_ble_control PRIVATE c_std_11)
if(MSVC)
  target_compile_options(test_ble_control PRIVATE /utf-8)
  target_compile_definitions(test_ble_control PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(test_ble_control PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_link_libraries(test_ble_control PRIVATE m)
endif()
add_test(NAME ble_control COMMAND test_ble_control)
# pin_log_redaction / ble_pin_logging 目标随鉴权协议一并移除 (2026-09-22):
# ble_ct_auth.c / ble_ct_pin.c 已删除, PIN 不再存在。
add_executable(test_arm_iap_codec test_arm_iap_codec.c ../../main/ota/arm_iap_codec.c)
target_include_directories(test_arm_iap_codec PRIVATE ../../main/ota)
if(MSVC)
  target_compile_options(test_arm_iap_codec PRIVATE /utf-8)
endif()
add_test(NAME arm_iap_codec COMMAND test_arm_iap_codec)
add_executable(test_dsp_iap_smoke test_dsp_iap_smoke.c ../../main/ota/arm_iap_codec.c)
target_include_directories(test_dsp_iap_smoke PRIVATE ../../main/ota)
if(MSVC)
  target_compile_options(test_dsp_iap_smoke PRIVATE /utf-8)
endif()
add_test(NAME dsp_iap_smoke COMMAND test_dsp_iap_smoke)
add_executable(test_ble_ota test_ble_ota.c ../../main/ota/ble_ota.c
  "${CJSON_DIR}/cJSON.c")
target_include_directories(test_ble_ota PRIVATE
  ble_stubs ../../main/ota "${CJSON_DIR}")
target_compile_features(test_ble_ota PRIVATE c_std_11)
if(MSVC)
  target_compile_options(test_ble_ota PRIVATE /utf-8)
  target_compile_definitions(test_ble_ota PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(test_ble_ota PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_link_libraries(test_ble_ota PRIVATE m)
endif()
add_test(NAME ble_ota COMMAND test_ble_ota)
add_executable(test_ble_info_modules test_ble_info_modules.c)
target_compile_definitions(test_ble_info_modules PRIVATE
  BLE_CT_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/../../components/ble_ct/ble_ct.c")
add_test(NAME ble_info_modules COMMAND test_ble_info_modules)

add_executable(test_debug_log_ring test_debug_log_ring.c
  ../../main/telemetry/debug_log_ring.c)
target_include_directories(test_debug_log_ring PRIVATE ../../main/telemetry)
target_compile_features(test_debug_log_ring PRIVATE c_std_11)
if(MSVC)
  target_compile_options(test_debug_log_ring PRIVATE /utf-8)
  target_compile_definitions(test_debug_log_ring PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(test_debug_log_ring PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
add_test(NAME debug_log_ring COMMAND test_debug_log_ring)

add_executable(test_offline_cache_quiet_lock test_offline_cache_quiet_lock.c)
target_compile_definitions(test_offline_cache_quiet_lock PRIVATE
  OFFLINE_CACHE_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/../../main/storage/offline_cache.c")
if(MSVC)
  target_compile_definitions(test_offline_cache_quiet_lock PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME offline_cache_quiet_lock COMMAND test_offline_cache_quiet_lock)

add_executable(test_cs_protocol_memory test_cs_protocol_memory.c)
target_compile_definitions(test_cs_protocol_memory PRIVATE
  CS_PROTOCOL_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/../../main/telemetry/cs_protocol.c")
if(MSVC)
  target_compile_definitions(test_cs_protocol_memory PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME cs_protocol_memory COMMAND test_cs_protocol_memory)

add_executable(test_cs_protocol_frame test_cs_protocol_frame.c
  ../../main/telemetry/cs_protocol.c)
target_include_directories(test_cs_protocol_frame PRIVATE ../../main/telemetry ../../main/ota)
if(MSVC)
  target_compile_options(test_cs_protocol_frame PRIVATE /utf-8)
endif()
add_test(NAME cs_protocol_frame COMMAND test_cs_protocol_frame)

add_executable(test_telemetry_bms test_telemetry_bms.c
  ../../main/telemetry/telemetry.c ../../main/mqtt/mqtt_envelope.c
  "${CJSON_DIR}/cJSON.c")
target_include_directories(test_telemetry_bms PRIVATE
  ../../main/telemetry ../../main/app ../../main/mqtt "${CJSON_DIR}")
target_compile_features(test_telemetry_bms PRIVATE c_std_11)
target_compile_definitions(test_telemetry_bms PRIVATE HOST_TEST)
if(MSVC)
  target_compile_options(test_telemetry_bms PRIVATE /utf-8)
else()
  target_link_libraries(test_telemetry_bms PRIVATE m)
endif()
add_test(NAME telemetry_bms COMMAND test_telemetry_bms)

add_executable(test_arm_bms_poll test_arm_bms_poll.c
  ../../main/telemetry/cs_protocol.c)
target_include_directories(test_arm_bms_poll PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}" ../../main/telemetry)
target_compile_features(test_arm_bms_poll PRIVATE c_std_11)
target_compile_definitions(test_arm_bms_poll PRIVATE HOST_TEST)
if(MSVC)
  target_compile_options(test_arm_bms_poll PRIVATE /utf-8)
endif()
add_test(NAME arm_bms_poll COMMAND test_arm_bms_poll)

add_executable(test_arm_ota_workspace_memory test_arm_ota_workspace_memory.c)
target_compile_definitions(test_arm_ota_workspace_memory PRIVATE
  ARM_OTA_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/../../main/ota/arm_ota.c"
  OTA_SERVICE_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/../../main/ota/ota_service.c")
if(MSVC)
  target_compile_definitions(test_arm_ota_workspace_memory PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME arm_ota_workspace_memory COMMAND test_arm_ota_workspace_memory)

add_executable(test_wifi_dns_fallback test_wifi_dns_fallback.c)
target_compile_definitions(test_wifi_dns_fallback PRIVATE
  WIFI_STA_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/../../main/wifi/wifi_sta.c")
if(MSVC)
  target_compile_definitions(test_wifi_dns_fallback PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME wifi_dns_fallback COMMAND test_wifi_dns_fallback)

add_executable(test_mqtt_broker_route test_mqtt_broker_route.c
  ../../main/mqtt/mqtt_broker_route.c)
target_include_directories(test_mqtt_broker_route PRIVATE ../../main/mqtt)
add_test(NAME mqtt_broker_route COMMAND test_mqtt_broker_route)

add_executable(test_mqtt_envelope test_mqtt_envelope.c ../../main/mqtt/mqtt_envelope.c)
target_include_directories(test_mqtt_envelope PRIVATE ../../main/mqtt)
add_test(NAME mqtt_envelope COMMAND test_mqtt_envelope)

add_executable(test_ota_transfer_policy test_ota_transfer_policy.c
  ../../main/ota/ota_transfer_policy.c)
target_include_directories(test_ota_transfer_policy PRIVATE ../../main/ota)
add_test(NAME ota_transfer_policy COMMAND test_ota_transfer_policy)
