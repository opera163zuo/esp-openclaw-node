

idf_component_register(
    SRC_DIRS
        "src"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "src"
    REQUIRES
        claw_paths
        claw_cap
        display_service
        display_ttf
        esp_board_manager
        esp_lcd
        esp_timer
        lvgl
    PRIV_REQUIRES
        esp_driver_gpio
)
