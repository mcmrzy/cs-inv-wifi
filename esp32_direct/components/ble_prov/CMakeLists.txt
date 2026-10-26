idf_component_register(
    SRCS "ble_prov.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        bt
)
