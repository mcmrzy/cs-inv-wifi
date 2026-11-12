# LVGL 8.4.0, ESP Component Registry release. This target builds offline.
file(GLOB_RECURSE lvgl_sources "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
list(FILTER lvgl_sources EXCLUDE REGEX "/qrcode/qrcodegen\\.c$")
idf_component_register(SRCS ${lvgl_sources} INCLUDE_DIRS "." "src")
