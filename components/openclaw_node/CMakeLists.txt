idf_component_register(
    SRCS "src/openclaw_node.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_websocket_client json nvs_flash mbedtls
    PRIV_REQUIRES esp_event freertos
)
