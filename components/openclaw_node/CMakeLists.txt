idf_component_register(
    SRCS "src/openclaw_node.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_websocket_client json esp_timer
    PRIV_REQUIRES esp_event freertos
)
