idf_component_register(
    SRCS "ble_prov.c"
    INCLUDE_DIRS "."
    REQUIRES
        wifi_provisioning
        esp_wifi
        esp_event
        nvs_flash
        bt
)
