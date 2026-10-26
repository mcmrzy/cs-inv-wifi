cmake_minimum_required(VERSION 3.16)

set(IDF_TARGET "esp32c3")
set(EXTRA_COMPONENT_DIRS "")

# Read version from version.txt (single source of truth)
file(READ "${CMAKE_SOURCE_DIR}/version.txt" VERSION_RAW)
string(STRIP "${VERSION_RAW}" PROJECT_VER)

# file(READ) 无依赖跟踪: 显式把 version.txt 注册为 configure 输入,
# 使其变化时自动重新 configure (否则增量编译沿用旧 PROJECT_VER)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_SOURCE_DIR}/version.txt")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cs_inv_direct_c3)
