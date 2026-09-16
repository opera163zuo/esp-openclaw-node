# This is the project CMakeLists.txt file for the test subproject
cmake_minimum_required(VERSION 3.16)

# On-target regression suite for the fork's changes.
#
# It lives under edge_agent rather than inside a component because it reuses
# edge_agent's already-downloaded managed components.
#
# openclaw_node is built with CONFIG_OPENCLAW_NODE_DEVICE_COMMANDS off and
# app_config needs only settings, so the board manager, the system UI, LVGL and
# the capability framework all stay out of the build: these tests run without a
# board, a display or the audio stack.
set(COMPONENTS main)

# Only the one managed component the transport actually needs. Pointing at the
# whole managed_components directory instead would make the build evaluate
# esp_board_manager's manifest, whose rules reference board Kconfig symbols that
# this build never defines.
set(EXTRA_COMPONENT_DIRS
    "../../../../components/openclaw_node"
    "../../../../components/common/settings"
    "../../components/app_config"
    "../../managed_components/espressif__esp_websocket_client"
)

if(NOT DEFINED IDF_TARGET)
    set(IDF_TARGET "esp32s3" CACHE STRING "ESP-IDF target")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(fork_regression_test)
