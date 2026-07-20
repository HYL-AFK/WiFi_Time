# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# ESP-IDF 5.3 does not expose lwIP's SNTP receive timeout through Kconfig.
add_compile_definitions(SNTP_RECV_TIMEOUT=3000)

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/VERSION" PROJECT_VER LIMIT_COUNT 1)
string(STRIP "${PROJECT_VER}" PROJECT_VER)
if(NOT PROJECT_VER MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION must use MAJOR.MINOR.PATCH format")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(WiTime)
