/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_CONFIG_STR_LEN        320
#define APP_CONFIG_TIMEZONE_LEN   32
/* Size of openclaw_device_family. This mirrors APP_CLAW_MODEL_LEN, which
 * app_claw uses for the same value; it is spelled out here so that this
 * component does not have to depend on app_claw just for a length. The two must
 * stay equal, because the stored NVS blob is sized by this one. */
#define APP_CONFIG_DEVICE_FAMILY_LEN 64

#define APP_WIFI_SSID             CONFIG_APP_WIFI_SSID
#define APP_WIFI_PASSWORD         CONFIG_APP_WIFI_PASSWORD

/**
 * Persisted device configuration.
 *
 * This device is an OpenClaw Native Node: it runs no Agent and no LLM, so the
 * stored configuration is limited to how it joins the network and how it
 * reaches its Gateway. Everything the Agent used to hold (LLM credentials, IM
 * platforms, search and ASR providers) lives on the Gateway instead.
 *
 * The OpenClaw fields are runtime configuration rather than compile-time
 * macros, so a device can be pointed at a different Gateway without a rebuild.
 */
typedef struct {
    /* Wi-Fi station credentials, filled in from the provisioning portal. */
    char wifi_ssid[APP_CONFIG_STR_LEN];
    char wifi_password[APP_CONFIG_STR_LEN];
    /* Optional fallback AP kept up for re-provisioning. */
    char ap_ssid[APP_CONFIG_STR_LEN];
    char ap_password[APP_CONFIG_STR_LEN];
    char ap_behavior[16];
    /* Optional password for the provisioning portal itself. Empty leaves the
     * portal open, which is what first-boot provisioning needs; setting it makes
     * every API request require HTTP Basic auth. */
    char portal_password[APP_CONFIG_STR_LEN];
    /* OpenClaw Gateway endpoint and node token. Empty URL disables the node. */
    char openclaw_gateway_url[APP_CONFIG_STR_LEN];
    char openclaw_gateway_token[APP_CONFIG_STR_LEN];
    /* Reported to the Gateway as the node's device family. */
    char openclaw_device_family[APP_CONFIG_DEVICE_FAMILY_LEN];
    /* Which capability groups and Lua modules this device exposes. */
    char enabled_cap_groups[APP_CONFIG_STR_LEN];
    char enabled_lua_modules[APP_CONFIG_STR_LEN];
    char time_timezone[APP_CONFIG_TIMEZONE_LEN];
} app_config_t;

esp_err_t app_config_init(void);
void app_config_load_defaults(app_config_t *config);
esp_err_t app_config_load(app_config_t *config);
esp_err_t app_config_save(const app_config_t *config);
esp_err_t app_config_validate_wifi(const app_config_t *config, const char **message);
esp_err_t app_config_validate_openclaw(const app_config_t *config, const char **message);
const char *app_config_get_timezone(const app_config_t *config);

#ifdef __cplusplus
}
#endif
