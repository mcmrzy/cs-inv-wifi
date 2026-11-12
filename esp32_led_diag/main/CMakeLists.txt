if(WS2812_RAINBOW)
    set(app_source rainbow.c)
else()
    set(app_source main.c)
endif()
idf_component_register(SRCS "${app_source}" INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_rmt esp_driver_gpio)
