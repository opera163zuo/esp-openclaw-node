set(FRONTEND_SOURCE_DIR        "${CMAKE_CURRENT_LIST_DIR}/frontend_source")
set(FRONTEND_DIST_HTML_GZ      "${FRONTEND_SOURCE_DIR}/dist/index.html.gz")
set(FRONTEND_PUBLIC_FAVICON_ICO  "${FRONTEND_SOURCE_DIR}/public/favicon.ico")

if(NOT EXISTS "${FRONTEND_DIST_HTML_GZ}")
    message(FATAL_ERROR
        "\n"
        "Frontend bundle not found: ${FRONTEND_DIST_HTML_GZ}\n"
        "Build the SolidJS UI before running idf.py build, e.g.:\n"
        "    cd ${FRONTEND_SOURCE_DIR}\n"
        "    pnpm install --frozen-lockfile\n"
        "    pnpm run build\n")
endif()

set(http_server_srcs
    "http_server_core.c"
    "http_server_assets.c"
    "http_server_config_api.c"
    "http_server_status_api.c"
    "http_server_files_api.c"
    "http_server_json.c"
    "http_server_utils.c"
)

# Only the unconditional dependencies belong here. app_claw and
# lua_module_http_server are declared in idf_component.yml instead, because a
# CONFIG_-conditional REQUIRES does not take effect: this file runs several
# times while sdkconfig is generated and the component is registered from the
# earliest pass, before those symbols exist.
set(http_server_requires
    app_config
    esp_http_server
    heap
    mbedtls
)

if(CONFIG_HTTP_SERVER_LUA_MODULES_API)
    list(APPEND http_server_srcs "http_server_lua_modules_api.c")
endif()

if(CONFIG_APP_CLAW_LUA_MODULE_HTTP_SERVER)
    list(APPEND http_server_srcs "http_server_lua_app_api.c")
endif()

idf_component_register(
    SRCS
        ${http_server_srcs}
    INCLUDE_DIRS
        "include"
    EMBED_FILES
        "${FRONTEND_DIST_HTML_GZ}"
        "${FRONTEND_PUBLIC_FAVICON_ICO}"
    REQUIRES
        ${http_server_requires}
)

# Rebuild the component whenever the bundled UI asset changes on disk.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FRONTEND_DIST_HTML_GZ}")

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
