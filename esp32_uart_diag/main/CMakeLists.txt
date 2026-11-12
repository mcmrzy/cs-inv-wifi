idf_component_register(SRCS "main.c" "diag_core.c" "../../esp32_modbus_panel/main/panel_modbus.c"
    INCLUDE_DIRS "." "../../esp32_modbus_panel/main"
    PRIV_REQUIRES esp_driver_uart esp_driver_gpio esp_driver_usb_serial_jtag esp_timer app_update)
