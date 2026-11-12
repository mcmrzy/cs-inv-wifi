cmake_minimum_required(VERSION 3.16)
set(IDF_TARGET "esp32c3")
option(PANEL_BMS_DIAGNOSTIC "Enable bounded UART1 BBJ diagnostics" OFF)
if(PANEL_BMS_DIAGNOSTIC)
    set(PROJECT_VER "0.4.3-bms-diag4")
else()
    set(PROJECT_VER "0.4.31-modbus-full")
endif()
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../esp32_direct/components")
if(NOT DEFINED SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/full/sdkconfig.defaults")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cs_inv_modbus_full)
# PIN configuration stays in the ignored build tree, never in command-line
# definitions or committed source. Regenerate on every build so an environment
# override cannot silently retain a prior build's configuration.
idf_build_get_property(panel_python PYTHON)
set(panel_pairing_generator "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_pairing_secret.py")
set(panel_pairing_legacy "${CMAKE_CURRENT_SOURCE_DIR}/../tools/pin_test_vectors.py")
set(panel_pairing_header "${CMAKE_BINARY_DIR}/generated/panel_pairing_secret.h")
execute_process(COMMAND "${panel_python}" "${panel_pairing_generator}" --output "${panel_pairing_header}" --legacy-source "${panel_pairing_legacy}"
    RESULT_VARIABLE panel_pairing_result)
if(NOT panel_pairing_result EQUAL 0)
    message(FATAL_ERROR "PIN build configuration unavailable")
endif()
add_custom_target(panel_pairing_config
    COMMAND "${panel_python}" "${panel_pairing_generator}" --output "${panel_pairing_header}" --legacy-source "${panel_pairing_legacy}"
    BYPRODUCTS "${panel_pairing_header}"
    VERBATIM)
idf_component_get_property(panel_main_lib main COMPONENT_LIB)
add_dependencies(${panel_main_lib} panel_pairing_config)
# Scope this substitution to cJSON, whose only scanf call parses its own %lg
# numeric output. Fail configuration if an IDF update changes that contract.
idf_component_get_property(panel_json_dir json COMPONENT_DIR)
file(READ "${panel_json_dir}/cJSON/cJSON.c" panel_json_source)
string(REGEX MATCHALL "sscanf\\(" panel_json_scan_calls "${panel_json_source}")
list(LENGTH panel_json_scan_calls panel_json_scan_count)
string(FIND "${panel_json_source}" "sscanf((char*)number_buffer, \"%lg\", &test)" panel_json_scan_position)
if(NOT panel_json_scan_count EQUAL 1 OR panel_json_scan_position LESS 0)
    message(FATAL_ERROR "Review cJSON numeric formatting before updating panel_json_scanf")
endif()
idf_component_get_property(panel_json_lib json COMPONENT_LIB)
target_compile_definitions(${panel_json_lib} PRIVATE sscanf=panel_json_scanf)
target_sources(${panel_json_lib} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/full/panel_json_number.c")
idf_component_get_property(panel_lvgl lvgl__lvgl COMPONENT_LIB)
target_compile_definitions(${panel_lvgl} PUBLIC "LV_CONF_PATH=${CMAKE_CURRENT_SOURCE_DIR}/full/lv_conf.h")


