idf_component_register(
    SRCS "ble_ct.c" "ble_ct_auth.c" "ble_ct_pin.c" "ble_ct_telemetry.c" "ble_ct_cmd.c" "ble_ct_clock.c"
    INCLUDE_DIRS "."
    REQUIRES ble_prov nvs_flash json mbedtls esp_timer
)
