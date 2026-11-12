# The IDF early dependency pass does not inherit arbitrary project options.
# Register the full component unconditionally so WiFi/BLE/TLS includes match it.
include("${CMAKE_CURRENT_LIST_DIR}/../full/component.cmake")
