idf_component_register(
    SRCS "src/openclaw_node.c" "src/openclaw_node_identity.c" "src/openclaw_node_device.c"
        "third_party/monocypher/monocypher.c"
        "third_party/monocypher/monocypher-ed25519.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "third_party/monocypher"
    REQUIRES esp_websocket_client json esp_timer nvs_flash esp_system esp_hw_support esp_psram mbedtls esp_netif esp_board_manager esp_codec_dev
    PRIV_REQUIRES esp_event freertos
)
