set(BASE "${CMAKE_CURRENT_LIST_DIR}/../../esp32_direct/main")
idf_component_register(
 SRCS
  "${CMAKE_CURRENT_LIST_DIR}/app_main.c"
  "${CMAKE_CURRENT_LIST_DIR}/firmware_identity.c"
  "${CMAKE_CURRENT_LIST_DIR}/panel_pairing.c"
  "${BASE}/app/admin_sn_command.c"
  "${BASE}/app/admin_sn_auth.c"
  "${BASE}/factory/factory_record.c"
  "${CMAKE_CURRENT_LIST_DIR}/factory_json.c"
  "${BASE}/factory/factory_pairing_record.c"
  "${BASE}/onboarding/cloud_binding.c"
  "${BASE}/onboarding/cloud_binding_codec.c"
  "${BASE}/onboarding/onboarding_state.c"
  "${BASE}/onboarding/onboarding_qr.c"
  "${BASE}/factory/factory_store.c"
  "${BASE}/factory/factory_identity.c"
  "${BASE}/factory/factory_wire.c"
  "${CMAKE_CURRENT_LIST_DIR}/factory_service.c"
  "${CMAKE_CURRENT_LIST_DIR}/direct_poll.c"
  "${CMAKE_CURRENT_LIST_DIR}/bms_bbj_frame.c"
  "${BASE}/direct/direct_protocol.c"
  "${BASE}/direct/direct_bms_identity.c"
  "${BASE}/direct/direct_bms_calibration.c"
  "${BASE}/direct/direct_iap.c"
  "${BASE}/direct/direct_display_sample.c"
  "${BASE}/direct/direct_display_events.c"
  "${BASE}/direct/direct_lcd_codec.c"
  "${CMAKE_CURRENT_LIST_DIR}/direct_panel.c"
  "${CMAKE_CURRENT_LIST_DIR}/direct_ui.c"
  "${CMAKE_CURRENT_LIST_DIR}/panel_lvgl.c"
  "${CMAKE_CURRENT_LIST_DIR}/panel_firmware_catalog.c"
  "${CMAKE_CURRENT_LIST_DIR}/panel_weather.c"
  "${CMAKE_CURRENT_LIST_DIR}/panel_home_art.c"
  "${BASE}/direct/direct_boot_logo.c"
  "${BASE}/direct/direct_home.c"
  "${CMAKE_CURRENT_LIST_DIR}/direct_ui_locale.c"
  "${CMAKE_CURRENT_LIST_DIR}/../main/panel_fonts.c"
  "${BASE}/direct/direct_keys.c"
  "${CMAKE_CURRENT_LIST_DIR}/panel_buzzer.c"
  "${BASE}/direct/direct_key_codec.c"
  "${CMAKE_CURRENT_LIST_DIR}/panel_preferences.c"
  "${CMAKE_CURRENT_LIST_DIR}/panel_ble_aux.c"
  "${BASE}/ota/arm_ota.c"
  "${BASE}/ota/arm_iap_codec.c"
  "${BASE}/telemetry/telemetry.c"
  "${BASE}/telemetry/debug_log.c"
  "${BASE}/telemetry/debug_log_ring.c"
  "${CMAKE_CURRENT_LIST_DIR}/cmd_handler.c"
  "${CMAKE_CURRENT_LIST_DIR}/panel_ble.c"
  "${BASE}/mqtt/mqtt_broker_route.c"
  "${BASE}/mqtt/mqtt_envelope.c"
  "${CMAKE_CURRENT_LIST_DIR}/mqtt_handler.c"
  "${BASE}/mqtt/ota_mqtt_adapter.c"
  "${CMAKE_CURRENT_LIST_DIR}/ota_service.c"
  "${BASE}/ota/ota_compat.c"
  "${BASE}/ota/ota_health.c"
  "${CMAKE_CURRENT_LIST_DIR}/ota_image_identity.c"
  "${BASE}/ota/ota_transfer_policy.c"
  "${BASE}/ota/ble_ota.c"
  "${BASE}/storage/offline_cache.c"
  "${BASE}/storage/nvs_storage.c"
  "${BASE}/ota/uart_ota.c"
  "${BASE}/wifi/wifi_sta.c"
  "${BASE}/wifi/sntp_sync.c"
  "${BASE}/telemetry/cs_protocol.c"
  "${CMAKE_CURRENT_LIST_DIR}/full_modbus_map.c"
  "${CMAKE_CURRENT_LIST_DIR}/../main/panel_bus.c"
  "${CMAKE_CURRENT_LIST_DIR}/../main/panel_control.c"
  "${CMAKE_CURRENT_LIST_DIR}/../main/panel_settings_query.c"
  "${CMAKE_CURRENT_LIST_DIR}/../main/panel_catalog.c"
  "${CMAKE_CURRENT_LIST_DIR}/../main/panel_model.c"
  "${CMAKE_CURRENT_LIST_DIR}/../main/panel_modbus.c"
  "${CMAKE_CURRENT_LIST_DIR}/../main/panel_ui.c"
  "${CMAKE_CURRENT_LIST_DIR}/../main/panel_locale.c"
  "${CMAKE_CURRENT_LIST_DIR}/../main/panel_indicators.c"
  "${CMAKE_CURRENT_LIST_DIR}/panel_indicators_hw.c"
  "${CMAKE_CURRENT_LIST_DIR}/panel_backlight.c"
  "${CMAKE_CURRENT_LIST_DIR}/../main/panel_display_settings.c"
  "${CMAKE_CURRENT_LIST_DIR}/../main/panel_alarm.c"
  "${CMAKE_CURRENT_LIST_DIR}/full_alarm_service.c"
 INCLUDE_DIRS
  "${CMAKE_CURRENT_LIST_DIR}"
  "${CMAKE_CURRENT_LIST_DIR}/../main"
  "${BASE}/app"
  "${BASE}/onboarding"
  "${BASE}/factory"
  "${BASE}/wifi"
  "${BASE}/mqtt"
  "${BASE}/ota"
  "${BASE}/telemetry"
  "${BASE}/direct"
  "${BASE}/storage"
 REQUIRES 
        nvs_flash
        esp_driver_usb_serial_jtag
        esp_driver_rmt
        esp_driver_ledc
        vfs
        spi_flash
        esp_wifi
        esp_netif
        esp_event
        esp_timer
        driver
        mqtt
        json
        lwip
        esp_system
        spiffs
        freertos
        esp_rom
        esp_http_client
        esp_https_ota
        esp-tls
        mbedtls
        app_update
        ble_prov
        ble_ct
        monocypher
        qrcodegen
        lvgl__lvgl

)
target_compile_definitions(${COMPONENT_LIB} PUBLIC FIRMWARE_VERSION="${PROJECT_VER}" PANEL_FULL_DIRECT=1)
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_BINARY_DIR}/generated")
# Preserve space in the existing 4 MB OTA layout for the BLE API.
set_source_files_properties(
    "${CMAKE_CURRENT_LIST_DIR}/panel_lvgl.c"
    "${CMAKE_CURRENT_LIST_DIR}/panel_ble.c"
    "${CMAKE_CURRENT_LIST_DIR}/panel_ble_aux.c"
    "${CMAKE_CURRENT_LIST_DIR}/full_alarm_service.c"
    "${CMAKE_CURRENT_LIST_DIR}/../main/panel_alarm.c"
    "${CMAKE_CURRENT_LIST_DIR}/../main/panel_catalog.c"
    "${CMAKE_CURRENT_LIST_DIR}/../main/panel_ui.c"
    PROPERTIES COMPILE_OPTIONS "-Oz"
)
if(PANEL_BMS_DIAGNOSTIC)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PANEL_BMS_DIAGNOSTIC=1)
    target_sources(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/bms_diagnostics.c")
    target_compile_options(${COMPONENT_LIB} PRIVATE -fstack-usage)
endif()




set_source_files_properties("${BASE}/storage/offline_cache.c" PROPERTIES COMPILE_OPTIONS "-Oz;-Wframe-larger-than=1536;-Werror=frame-larger-than=1536")
