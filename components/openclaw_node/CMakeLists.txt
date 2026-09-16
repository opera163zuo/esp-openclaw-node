set(OPENCLAW_NODE_SRCS
    "src/openclaw_node.c"
    "src/openclaw_node_identity.c"
    "third_party/monocypher/monocypher.c"
    "third_party/monocypher/monocypher-ed25519.c"
)

set(OPENCLAW_NODE_REQUIRES
    esp_websocket_client
    json
    esp_timer
    nvs_flash
    esp_system
    esp_hw_support
    esp_psram
    mbedtls
    esp_netif
)

# The device command table is the only part that needs the board manager, the
# system UI and the capability framework. Keeping it behind a switch lets the
# transport be built - and tested on target - on its own; the stub keeps the
# public API linkable for callers such as the application's main.
#
# Those dependencies are declared in idf_component.yml rather than appended to
# REQUIRES here, because a CONFIG_-conditional REQUIRES does not take effect:
# this file runs several times while sdkconfig is generated and the component is
# registered from the earliest pass, before the symbol exists.
if(CONFIG_OPENCLAW_NODE_DEVICE_COMMANDS)
    list(APPEND OPENCLAW_NODE_SRCS "src/openclaw_node_device.c")
else()
    list(APPEND OPENCLAW_NODE_SRCS "src/openclaw_node_device_stub.c")
endif()

idf_component_register(
    SRCS ${OPENCLAW_NODE_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "third_party/monocypher"
    REQUIRES ${OPENCLAW_NODE_REQUIRES}
    PRIV_REQUIRES esp_event freertos
)
