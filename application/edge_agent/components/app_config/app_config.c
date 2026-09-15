/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "settings_store.h"

typedef struct {
    const char *key;
    const char *default_value;
    size_t offset;
    size_t size;
} app_config_field_t;

#define APP_CONFIG_FIELD(member, nvs_key, default_literal) \
    { nvs_key, default_literal, offsetof(app_config_t, member), sizeof(((app_config_t *)0)->member) }

#define APP_DEFAULT_ENABLED_CAP_GROUPS       ""
#define APP_DEFAULT_ENABLED_LUA_MODULES      ""
#define APP_DEFAULT_TIME_TIMEZONE            "CST-8"

static const app_config_field_t s_fields[] = {
    APP_CONFIG_FIELD(wifi_ssid, "wifi_ssid", APP_WIFI_SSID),
    APP_CONFIG_FIELD(wifi_password, "wifi_password", APP_WIFI_PASSWORD),
    APP_CONFIG_FIELD(ap_ssid, "ap_ssid", ""),
    APP_CONFIG_FIELD(ap_password, "ap_password", ""),
    APP_CONFIG_FIELD(ap_behavior, "ap_behavior", "keep"),
    APP_CONFIG_FIELD(portal_password, "portal_pwd", ""),
    APP_CONFIG_FIELD(openclaw_gateway_url, "oc_gw_url", ""),
    APP_CONFIG_FIELD(openclaw_gateway_token, "oc_gw_token", ""),
    APP_CONFIG_FIELD(openclaw_device_family, "oc_dev_family", "m5stack-sticks3"),
    APP_CONFIG_FIELD(enabled_cap_groups, "en_cap_groups", APP_DEFAULT_ENABLED_CAP_GROUPS),
    APP_CONFIG_FIELD(enabled_lua_modules, "en_lua_mods", APP_DEFAULT_ENABLED_LUA_MODULES),
    APP_CONFIG_FIELD(time_timezone, "time_timezone", APP_DEFAULT_TIME_TIMEZONE),
};

// for backward compatibility, migrate from old settings to new settings
static inline char *app_config_field_ptr(app_config_t *config, const app_config_field_t *field)
{
    return (char *)config + field->offset;
}

static inline const char *app_config_field_cptr(const app_config_t *config, const app_config_field_t *field)
{
    return (const char *)config + field->offset;
}

static bool app_config_ap_behavior_is_valid(const char *ap_behavior)
{
    return !ap_behavior || ap_behavior[0] == '\0' ||
           strcmp(ap_behavior, "keep") == 0 ||
           strcmp(ap_behavior, "close_on_sta") == 0;
}

esp_err_t app_config_init(void)
{
    esp_err_t err = settings_store_init(&(settings_store_config_t) {
        .namespace_name = "app",
    });
    if (err != ESP_OK) {
        return err;
    }
    return ESP_OK;
}

void app_config_load_defaults(app_config_t *config)
{
    if (!config) {
        return;
    }

    memset(config, 0, sizeof(*config));

    for (size_t i = 0; i < sizeof(s_fields) / sizeof(s_fields[0]); ++i) {
        strlcpy(app_config_field_ptr(config, &s_fields[i]),
                s_fields[i].default_value ? s_fields[i].default_value : "",
                s_fields[i].size);
    }
}

esp_err_t app_config_load(app_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    app_config_load_defaults(config);

    for (size_t i = 0; i < sizeof(s_fields) / sizeof(s_fields[0]); ++i) {
        esp_err_t err = settings_store_get_string(s_fields[i].key,
                                                  app_config_field_ptr(config, &s_fields[i]),
                                                  s_fields[i].size,
                                                  s_fields[i].default_value);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

esp_err_t app_config_save(const app_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < sizeof(s_fields) / sizeof(s_fields[0]); ++i) {
        esp_err_t err = settings_store_set_string(s_fields[i].key,
                                                  app_config_field_cptr(config, &s_fields[i]));
        if (err != ESP_OK) {
            return err;
        }
    }

    return settings_store_commit();
}

esp_err_t app_config_validate_wifi(const app_config_t *config, const char **message)
{
    if (message) {
        *message = NULL;
    }
    if (!config) {
        if (message) {
            *message = "Missing Wi-Fi configuration";
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (config->wifi_ssid[0] != '\0' && strlen(config->wifi_ssid) >= 32) {
        if (message) {
            *message = "wifi_ssid must be 1-31 characters";
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (config->wifi_password[0] != '\0') {
        size_t wifi_password_len = strlen(config->wifi_password);
        if (wifi_password_len < 8 || wifi_password_len > 63) {
            if (message) {
                *message = "wifi_password must be empty or 8-63 characters";
            }
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (config->ap_password[0] != '\0') {
        size_t ap_password_len = strlen(config->ap_password);
        if (ap_password_len < 8 || ap_password_len > 63) {
            if (message) {
                *message = "ap_password must be empty or 8-63 characters";
            }
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (config->ap_ssid[0] != '\0' && strlen(config->ap_ssid) > 32) {
        if (message) {
            *message = "ap_ssid must be 1-32 characters";
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (config->portal_password[0] != '\0') {
        size_t portal_password_len = strlen(config->portal_password);
        if (portal_password_len < 8 || portal_password_len > 63) {
            if (message) {
                *message = "portal_password must be empty or 8-63 characters";
            }
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (!app_config_ap_behavior_is_valid(config->ap_behavior)) {
        if (message) {
            *message = "ap_behavior must be keep or close_on_sta";
        }
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t app_config_validate_openclaw(const app_config_t *config, const char **message)
{
    if (message) {
        *message = NULL;
    }
    if (!config) {
        if (message) {
            *message = "Missing OpenClaw configuration";
        }
        return ESP_ERR_INVALID_ARG;
    }

    /* Empty means explicitly disabled. A non-empty endpoint must be a
     * WebSocket URL; accepting arbitrary strings here only delays the error
     * until the transport task starts and makes provisioning confusing. */
    if (config->openclaw_gateway_url[0] != '\0' &&
        strncmp(config->openclaw_gateway_url, "ws://", 5) != 0 &&
        strncmp(config->openclaw_gateway_url, "wss://", 6) != 0) {
        if (message) {
            *message = "openclaw_gateway_url must start with ws:// or wss://";
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (config->openclaw_gateway_url[0] != '\0' && strlen(config->openclaw_gateway_url) >= APP_CONFIG_STR_LEN) {
        if (message) {
            *message = "openclaw_gateway_url is too long";
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (config->openclaw_device_family[0] == '\0') {
        if (message) {
            *message = "openclaw_device_family must not be empty";
        }
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

void app_config_to_claw(const app_config_t *config, app_claw_config_t *out)
{
    if (!config || !out) {
        return;
    }

    memset(out, 0, sizeof(*out));

    strlcpy(out->enabled_cap_groups, config->enabled_cap_groups, sizeof(out->enabled_cap_groups));
    strlcpy(out->enabled_lua_modules, config->enabled_lua_modules, sizeof(out->enabled_lua_modules));
}

const char *app_config_get_timezone(const app_config_t *config)
{
    return config ? config->time_timezone : NULL;
}
