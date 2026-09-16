# This is the project CMakeLists.txt file for the test subproject
cmake_minimum_required(VERSION 3.16)

# On-target regression suite for the fork's changes.
#
# It lives under edge_agent rather than inside a component because it reuses
# edge_agent's already-downloaded managed components.
#
# openclaw_node is built with CONFIG_OPENCLAW_NODE_DEVICE_COMMANDS off and
# http_server with CONFIG_HTTP_SERVER_LUA_MODULES_API off, and app_config needs
# only settings, so the board manager, the system UI, LVGL, app_claw and the
# capability framework all stay out of the build: these tests run without a
# board, a display or the audio stack.
set(COMPONENTS main)

# The WebSocket client comes from main/idf_component.yml, so the component
# manager fetches it into this app's own managed_components. Reaching into
# edge_agent's copy would only work on a machine that had already built
# edge_agent, since managed_components is not checked in.
set(EXTRA_COMPONENT_DIRS
    "../../../../components/openclaw_node"
    "../../../../components/common/settings"
    "../../../../components/common/time_sync"
    "../../components/app_config"
    "../../components/http_server"
)

if(NOT DEFINED IDF_TARGET)
    set(IDF_TARGET "esp32s3" CACHE STRING "ESP-IDF target")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(fork_regression_test)
