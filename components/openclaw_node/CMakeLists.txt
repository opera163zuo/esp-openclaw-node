idf_component_register(
    SRCS "src/openclaw_node.c" "src/openclaw_node_identity.c" "src/openclaw_node_device.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_websocket_client json esp_timer nvs_flash mbedtls esp_system esp_hw_support esp_psram
    PRIV_REQUIRES esp_event freertos
)
