/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_claw.h"
#include "app_fs.h"
#include "claw_paths.h"
#include "edge_agent_version.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include "wifi_manager.h"
#include "time.h"
#include "nvs_flash.h"
#include "http_server.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_board_manager_includes.h"
#include "captive_dns.h"
#include "cmd_wifi.h"
#include "openclaw_node_identity.h"
#include "openclaw_node_device.h"
#include "openclaw_node.h"
#include "freertos/task.h"
#include "app_config.h"

#define APP_ENABLE_MEM_LOG        (0)

static const char *TAG = "app";

static app_config_t *s_config;
static app_claw_config_t *s_claw_config;

static void start_openclaw_node_if_configured(const app_config_t *app_config);

static esp_err_t app_allocate_runtime_state(void)
{
    if (!s_config) {
        s_config = calloc(1, sizeof(*s_config));
    }
    if (!s_claw_config) {
        s_claw_config = calloc(1, sizeof(*s_claw_config));
    }

    ESP_RETURN_ON_FALSE(s_config && s_claw_config, ESP_ERR_NO_MEM, TAG,
                        "Failed to allocate runtime state");

    return ESP_OK;
}

static void app_free_runtime_state(void)
{
    free(s_claw_config);
    s_claw_config = NULL;

    free(s_config);
    s_config = NULL;
}

/* The provisioning AP is WPA2 when `ap_password` is set and completely open
 * when it is not (see wifi_manager.c). An open AP lets anyone within radio
 * range reach the unauthenticated config portal and repoint the device at
 * another Gateway, so a password is generated on first boot instead of leaving
 * the portal open. The value is persisted, so it stays stable across reboots,
 * and it is printed below for the operator. Clearing it from the portal still
 * produces an open AP for anyone who deliberately wants one. */
static void ensure_ap_password(app_config_t *config)
{
    if (config->ap_password[0] != '\0') {
        return;
    }

    /* 12 characters from an unambiguous alphabet (~59 bits). Long enough that
     * guessing is not the weak link, short enough to retype from the log. */
    static const char alphabet[] = "abcdefghijkmnpqrstuvwxyz23456789";
    char generated[13];
    for (size_t i = 0; i < sizeof(generated) - 1; i++) {
        generated[i] = alphabet[esp_random() % (sizeof(alphabet) - 1)];
    }
    generated[sizeof(generated) - 1] = '\0';

    strlcpy(config->ap_password, generated, sizeof(config->ap_password));
    esp_err_t err = app_config_save(config);
    ESP_LOGW(TAG, "No AP password set; generated one for this device: %s", generated);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AP password could not be persisted, it will change on reboot: %s",
                 esp_err_to_name(err));
    }
}

static void log_wifi_startup_config(const app_config_t *config)
{
    ESP_LOGI(TAG,
             "Wi-Fi startup STA: ssid=%s pwd_len=%u",
             config->wifi_ssid[0] ? config->wifi_ssid : "(empty)",
             (unsigned)strlen(config->wifi_password));

    ESP_LOGI(TAG,
             "Wi-Fi startup AP: ssid=%s pwd_len=%u behavior=%s",
             config->ap_ssid[0] ? config->ap_ssid : "(auto:mac-suffix)",
             (unsigned)strlen(config->ap_password),
             config->ap_behavior[0] ? config->ap_behavior : "keep");
}

static void on_wifi_state_changed(bool connected, void *user_ctx)
{
    (void)user_ctx;

    wifi_manager_status_t status = {0};
    wifi_manager_get_status(&status);
    const char *ap_ssid = status.ap_active ? status.ap_ssid : NULL;

    ESP_LOGI(TAG, "Wi-Fi state: sta_connected=%d ap_active=%d mode=%s ap_ssid=%s",
             connected,
             status.ap_active,
             status.mode ? status.mode : "off",
             ap_ssid ? ap_ssid : "(none)");

    esp_err_t err = app_claw_set_network_status(connected, ap_ssid);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to update network UI: %s", esp_err_to_name(err));
    }
}

static esp_err_t main_load_config(app_config_t *config)
{
    return app_config_load(config);
}

static esp_err_t main_save_config(const app_config_t *config)
{
    esp_err_t err;
    app_claw_config_t *claw_config = NULL;

    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_ERROR(app_config_validate_wifi(config, NULL), TAG, "Invalid Wi-Fi config");
    ESP_RETURN_ON_ERROR(app_config_validate_openclaw(config, NULL), TAG, "Invalid OpenClaw config");

    err = app_config_save(config);
    if (err != ESP_OK) {
        return err;
    }

    claw_config = calloc(1, sizeof(*claw_config));
    if (!claw_config) {
        ESP_LOGW(TAG, "Failed to allocate runtime config");
        return ESP_OK;
    }
    app_config_to_claw(config, claw_config);
    err = app_claw_update_config(claw_config);
    free(claw_config);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Failed to update runtime config: %s", esp_err_to_name(err));
    }

    /* The Native Node stores pointers to the app config. Copy the new values
     * before restarting the WebSocket client so Gateway changes take effect
     * immediately, without rebuilding or reflashing the firmware. */
    if (s_config) {
        *s_config = *config;
        openclaw_node_stop();
        start_openclaw_node_if_configured(s_config);
    }
    return ESP_OK;
}

static esp_err_t main_get_wifi_status(http_server_wifi_status_t *status)
{
    ESP_RETURN_ON_FALSE(status, ESP_ERR_INVALID_ARG, TAG, "status is NULL");

    wifi_manager_status_t wifi_status = {0};
    wifi_manager_get_status(&wifi_status);
    status->wifi_connected = wifi_status.sta_connected;
    status->ip = wifi_status.sta_ip;
    status->ap_active = wifi_status.ap_active;
    status->ap_ssid = wifi_status.ap_ssid;
    status->ap_ip = wifi_status.ap_ip;
    status->wifi_mode = wifi_status.mode;
    return ESP_OK;
}

static void main_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t main_restart_device(void)
{
    BaseType_t ok = xTaskCreate(main_restart_task, "http_restart", 2048, NULL, 5, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "Failed to create restart task");
    return ESP_OK;
}

static esp_err_t openclaw_node_sign_cb(const char *payload,
                                       char *signature_out,
                                       size_t signature_size,
                                       void *user_ctx)
{
    (void)user_ctx;
    return openclaw_node_identity_sign_b64url(payload, signature_out, signature_size);
}

static void start_openclaw_node_if_configured(const app_config_t *app_config)
{
    if (!app_config || app_config->openclaw_gateway_url[0] == '\0') {
        ESP_LOGI(TAG, "OpenClaw Native Node disabled: no Gateway URL configured");
        return;
    }
    size_t command_count = 0;
    const char *const *commands = openclaw_node_device_commands(&command_count);
    /* The Native Node task uses these strings after this function returns. */
    static char device_id[OPENCLAW_NODE_ED25519_PUBLIC_KEY_LEN * 2 + 1];
    static char public_key[64];
    if (openclaw_node_identity_get_id(device_id, sizeof(device_id)) != ESP_OK ||
        openclaw_node_identity_get_public_key_b64url(public_key, sizeof(public_key)) != ESP_OK) {
        ESP_LOGW(TAG, "Native Node not started: identity is unavailable");
        return;
    }
    openclaw_node_config_t config = {
        .gateway_url = app_config->openclaw_gateway_url,
        .gateway_token = app_config->openclaw_gateway_token[0] ? app_config->openclaw_gateway_token : NULL,
        .device_id = device_id,
        .public_key_b64url = public_key,
        .client_id = "node-host",
        .client_version = "0.1.0",
        .platform = "esp32",
        .device_family = app_config->openclaw_device_family[0] ? app_config->openclaw_device_family : "esp32",
        .commands = commands,
        .command_count = command_count,
        .sign_cb = openclaw_node_sign_cb,
        .command_cb = openclaw_node_device_command,
    };
    esp_err_t err = openclaw_node_start(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Native Node start failed: %s", esp_err_to_name(err));
    }
}

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t init_timezone(const char *timezone)
{
    esp_err_t ret = ESP_OK;

    ESP_GOTO_ON_FALSE(timezone && timezone[0] != '\0', ESP_ERR_INVALID_ARG, tz_default, TAG,
                      "Timezone is empty.");
    ESP_GOTO_ON_FALSE(setenv("TZ", timezone, 1) == 0, ESP_FAIL, tz_default, TAG,
                      "Failed to set TZ env");
    tzset();
    ESP_LOGI(TAG, "Timezone set to %s", timezone);
    return ESP_OK;

tz_default:
    assert(setenv("TZ", "CST-8", 1) == 0);
    tzset();
    ESP_LOGI(TAG, "Timezone set to default: CST-8");
    return ret;
}

#if APP_ENABLE_MEM_LOG

static void print_task_stack_info(void)
{
#ifdef CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    static TaskStatus_t s_task_status_snapshot[24];
    UBaseType_t count = uxTaskGetSystemState(s_task_status_snapshot,
                                             sizeof(s_task_status_snapshot) / sizeof(s_task_status_snapshot[0]),
                                             NULL);

    for (UBaseType_t i = 0; i < count; i++) {
        ESP_LOGI(TAG,
                 "Task %s  %u",
                 s_task_status_snapshot[i].pcTaskName,
                 s_task_status_snapshot[i].usStackHighWaterMark);
    }
#endif
}

/* Periodic task: print internal free, minimum free, and PSRAM free every 20s */
static void memory_monitor_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t internal_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "Memory: internal_free=%u bytes, internal_min_free=%u bytes, psram_free=%u bytes",
                 (unsigned)internal_free, (unsigned)internal_min, (unsigned)psram_free);
        print_task_stack_info();
    }
}

#endif

void app_main(void)
{
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);
    esp_log_level_set("http_reuse", ESP_LOG_WARN);

    ESP_LOGI(TAG, "Starting app");
    ESP_LOGI(TAG, "Edge Agent version: %s", edge_agent_get_version());
    ESP_ERROR_CHECK(app_allocate_runtime_state());
    ESP_ERROR_CHECK(init_nvs());
    esp_err_t identity_err = openclaw_node_identity_init();
    if (identity_err != ESP_OK) {
        ESP_LOGW(TAG, "OpenClaw identity unavailable: %s (Native Node remains disabled)",
                 esp_err_to_name(identity_err));
    } else {
        char identity_id[OPENCLAW_NODE_ED25519_PUBLIC_KEY_LEN * 2 + 1];
        if (openclaw_node_identity_get_id(identity_id, sizeof(identity_id)) == ESP_OK) {
            ESP_LOGI(TAG, "OpenClaw identity ready: device_id=%s", identity_id);
        }
    }
    ESP_ERROR_CHECK(app_config_init());
    ESP_ERROR_CHECK(app_config_load(s_config));
    ensure_ap_password(s_config);
    app_config_to_claw(s_config, s_claw_config);
    init_timezone(app_config_get_timezone(s_config)); // no need to check error
    ESP_ERROR_CHECK(esp_board_manager_init());
    ESP_ERROR_CHECK(app_fs_init());

    /* Publish the resolved storage roots so any component can compose paths
     * without knowing whether data lives on flash or an SD card. */
    ESP_ERROR_CHECK(claw_paths_set(CLAW_PATH_DATA, app_fs_storage_base_path()));
    ESP_ERROR_CHECK(claw_paths_set(CLAW_PATH_SYSTEM, app_fs_system_base_path()));

    ESP_ERROR_CHECK(wifi_manager_init());

    ESP_ERROR_CHECK(app_claw_ui_start());

    ESP_ERROR_CHECK(http_server_init(&(http_server_config_t) {
        .storage_base_path = app_fs_storage_base_path(),
        .services = {
            .load_config = main_load_config,
            .save_config = main_save_config,
            .get_wifi_status = main_get_wifi_status,
            .restart_device = main_restart_device,
        },
    }));
    ESP_ERROR_CHECK(wifi_manager_register_state_callback(on_wifi_state_changed, NULL));

    log_wifi_startup_config(s_config);

    esp_err_t wifi_err = wifi_manager_start(&(wifi_manager_config_t) {
        .sta_ssid = s_config->wifi_ssid,
        .sta_password = s_config->wifi_password,
        .ap_ssid = s_config->ap_ssid[0] ? s_config->ap_ssid : NULL,
        .ap_password = s_config->ap_password[0] ? s_config->ap_password : NULL,
        .ap_behavior = s_config->ap_behavior,
    });
    if (wifi_err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi start failed: %s", esp_err_to_name(wifi_err));
    } else {
        ESP_ERROR_CHECK(http_server_start());
        if (captive_dns_start(&(captive_dns_config_t) {
                .ap_netif = wifi_manager_get_ap_netif(),
                .configure_dhcp_dns = true,
            }) != ESP_OK) {
            ESP_LOGW(TAG, "Captive DNS could not start, portal pop-up disabled");
        }

        if (s_config->wifi_ssid[0] != '\0') {
            esp_err_t wait_err = wifi_manager_wait_connected(30000);
            if (wait_err == ESP_OK) {
                wifi_manager_status_t status = {0};
                wifi_manager_get_status(&status);
                ESP_LOGI(TAG, "Wi-Fi STA ready: %s", status.sta_ip);
            } else if (wait_err == ESP_ERR_TIMEOUT) {
                wifi_manager_status_t status = {0};
                wifi_manager_get_status(&status);
                ESP_LOGW(TAG,
                         "Wi-Fi STA not connected within wait window; retrying in background: mode=%s ap_active=%d ap_ip=%s",
                         status.mode ? status.mode : "off",
                         status.ap_active,
                         status.ap_ip ? status.ap_ip : "0.0.0.0");
            } else {
                ESP_LOGW(TAG, "Wi-Fi STA wait returned error: %s", esp_err_to_name(wait_err));
            }
        }

        wifi_manager_status_t status = {0};
        wifi_manager_get_status(&status);
        if (status.ap_active) {
            const char *portal_auth = s_config->ap_password[0] ? "wpa2" : "open";
            ESP_LOGW(TAG,
                     "*** Provisioning portal: SSID=\"%s\" (auth=%s) IP=%s URL=http://%s/ ***",
                     status.ap_ssid,
                     portal_auth,
                     status.ap_ip,
                     status.ap_ip);
        }
    }

    ESP_ERROR_CHECK(app_claw_start(s_claw_config));
    start_openclaw_node_if_configured(s_config);

    register_wifi_command();

#if APP_ENABLE_MEM_LOG
    /* Start memory monitor: print internal free, min free, PSRAM free every 20s */
    xTaskCreate(memory_monitor_task, "mem_mon", 4096, NULL, 1, NULL);
#endif

    app_free_runtime_state();
}
