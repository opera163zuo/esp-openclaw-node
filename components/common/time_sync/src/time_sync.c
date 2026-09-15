/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "time_sync.h"

#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "time_sync";

/* 2024-01-01T00:00:00Z. Anything below this means the clock was never set. */
#define TIME_SYNC_MIN_VALID_EPOCH 1704067200

#define TIME_SYNC_PRIMARY_SERVER   "pool.ntp.org"
#define TIME_SYNC_SECONDARY_SERVER "time.windows.com"
#define TIME_SYNC_SYNC_WAIT_MS     3000
#define TIME_SYNC_SYNC_RETRIES     15
#define TIME_SYNC_RETRY_DELAY_MS   5000
#define TIME_SYNC_TASK_STACK       4096
#define TIME_SYNC_TASK_PRIORITY    5

static TaskHandle_t s_task;
static bool s_running;

bool time_sync_is_valid(void)
{
    return time(NULL) >= TIME_SYNC_MIN_VALID_EPOCH;
}

static void time_sync_notification_cb(struct timeval *tv)
{
    (void)tv;
    ESP_LOGI(TAG, "SNTP time synchronization event received");
}

/* The interface may not have an address yet when the worker first runs. */
static bool time_sync_network_is_ready(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {0};

    if (netif == NULL) {
        return false;
    }
    return esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0;
}

static esp_err_t time_sync_run_once(void)
{
#if CONFIG_LWIP_SNTP_MAX_SERVERS > 1
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        2, ESP_SNTP_SERVER_LIST(TIME_SYNC_PRIMARY_SERVER, TIME_SYNC_SECONDARY_SERVER));
#else
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(TIME_SYNC_PRIMARY_SERVER);
#endif
    config.sync_cb = time_sync_notification_cb;

    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sntp init failed: %s", esp_err_to_name(err));
        return err;
    }

    int retry = 0;
    esp_err_t wait_err = ESP_ERR_TIMEOUT;
    while ((wait_err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(TIME_SYNC_SYNC_WAIT_MS))) == ESP_ERR_TIMEOUT &&
           ++retry < TIME_SYNC_SYNC_RETRIES) {
        ESP_LOGI(TAG, "waiting for system time (%d/%d)", retry, TIME_SYNC_SYNC_RETRIES);
    }

    esp_netif_sntp_deinit();

    if (wait_err != ESP_OK) {
        ESP_LOGW(TAG, "sntp sync did not complete: %s", esp_err_to_name(wait_err));
        return wait_err;
    }
    ESP_LOGI(TAG, "system time synchronized");
    return ESP_OK;
}

static void time_sync_task(void *arg)
{
    (void)arg;

    while (s_running) {
        if (time_sync_is_valid()) {
            /* Nothing left to do until the next reboot. */
            s_running = false;
            break;
        }
        if (!time_sync_network_is_ready()) {
            vTaskDelay(pdMS_TO_TICKS(TIME_SYNC_RETRY_DELAY_MS));
            continue;
        }
        if (time_sync_run_once() != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(TIME_SYNC_RETRY_DELAY_MS));
        }
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t time_sync_start(void)
{
    if (s_running || s_task != NULL) {
        return ESP_OK;
    }
    s_running = true;
    if (xTaskCreate(time_sync_task, "time_sync", TIME_SYNC_TASK_STACK, NULL,
                    TIME_SYNC_TASK_PRIORITY, &s_task) != pdPASS) {
        s_running = false;
        s_task = NULL;
        ESP_LOGE(TAG, "failed to create worker task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void time_sync_stop(void)
{
    s_running = false;
    /* The worker deletes itself; it observes s_running on its next tick. */
}
