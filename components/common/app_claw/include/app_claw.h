/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_CLAW_STR_LEN              320
#define APP_CLAW_SHORT_STR_LEN        32
#define APP_CLAW_MODEL_LEN            64
#define APP_CLAW_TIMEOUT_LEN          16
#define APP_CLAW_PATH_LEN             64
#define APP_CLAW_FILE_PATH_LEN        96

/**
 * Runtime configuration for the application shell.
 *
 * The device runs no Agent and no LLM, so this only carries what the device
 * still needs locally: which capability groups to register and which Lua
 * modules to expose. Everything that used to configure the Agent (LLM
 * credentials, IM platforms, search and ASR providers) now lives on the
 * OpenClaw Gateway.
 */
typedef struct {
    char enabled_cap_groups[APP_CLAW_STR_LEN];
    char enabled_lua_modules[APP_CLAW_STR_LEN];
} app_claw_config_t;

esp_err_t app_claw_start(const app_claw_config_t *config);
esp_err_t app_claw_update_config(const app_claw_config_t *config);
esp_err_t app_claw_get_config(app_claw_config_t *out_config);
esp_err_t app_claw_apply_config(const app_claw_config_t *config);
esp_err_t app_claw_ui_start(void);
esp_err_t app_claw_set_network_status(bool sta_connected, const char *ap_ssid);

#ifdef __cplusplus
}
#endif
