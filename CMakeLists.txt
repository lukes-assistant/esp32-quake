# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Olimex ESP32-P4-PC: use the HDMI-capable ESP32-P4 BSP snapshot from Olimex's
# p4_board_demo (github.com/OLIMEX/ESP32-P4-PC), validated on this board.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/components/esp-bsp/bsp/esp32_p4_function_ev_board"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(quake)

# esp_video (pulled in by the BSP) uses deprecated ESP-IDF 5.5 enum aliases
idf_component_get_property(esp_video_lib espressif__esp_video COMPONENT_LIB)
target_compile_options(${esp_video_lib} PRIVATE -Wno-deprecated-declarations)
