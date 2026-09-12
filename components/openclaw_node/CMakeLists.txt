idf_component_register(
    SRCS "src/openclaw_node.c" "src/openclaw_node_identity.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_websocket_client json esp_timer nvs_flash mbedtls
    PRIV_REQUIRES esp_event freertos
)
