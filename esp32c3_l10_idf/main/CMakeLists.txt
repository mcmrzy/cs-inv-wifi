idf_component_register(
    SRCS
        "app/app_main.c"
        "telemetry/arm_poll.c"
        "ota/arm_ota.c"
        "ota/arm_iap_codec.c"
        "telemetry/telemetry.c"
        "telemetry/debug_log.c"
        "telemetry/debug_log_ring.c"
        "telemetry/cmd_handler.c"
        "mqtt/mqtt_broker_route.c"
        "mqtt/mqtt_envelope.c"
        "mqtt/mqtt_handler.c"
        "mqtt/ota_mqtt_adapter.c"
        "ota/ota_service.c"
        "ota/ota_transfer_policy.c"
        "ota/ble_ota.c"
        "storage/offline_cache.c"
        "storage/nvs_storage.c"
        "ota/uart_ota.c"
        "wifi/wifi_sta.c"
        "wifi/sntp_sync.c"
        "telemetry/cs_protocol.c"
    INCLUDE_DIRS
        "app"
        "wifi"
        "mqtt"
        "ota"
        "telemetry"
        "storage"
    REQUIRES
        nvs_flash
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
)

# Inject version from root version.txt into code as FIRMWARE_VERSION.
# 直接用 PROJECT_VER (如 1.5.1): 需与固件记录/升级包比对用的版本号一致,
# 不再带 V 前缀和 .L10 后缀 (设备遥测上报值要能和 firmware_versions 对上)。
target_compile_definitions(${COMPONENT_LIB} PUBLIC FIRMWARE_VERSION="${PROJECT_VER}")
