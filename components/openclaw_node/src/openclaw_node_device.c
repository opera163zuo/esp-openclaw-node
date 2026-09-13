#include "openclaw_node_device.h"
#include "openclaw_node_identity.h"
#include "esp_chip_info.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_netif.h"
#include "esp_board_manager.h"
#include "esp_board_manager_defs.h"
#include "esp_board_periph.h"
#include "periph_ledc.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_codec_dev.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>

static const char *s_commands[] = {
    OPENCLAW_NODE_DEVICE_COMMAND_INFO,
    OPENCLAW_NODE_DEVICE_COMMAND_STATUS,
    OPENCLAW_NODE_DEVICE_COMMAND_NETWORK,
    OPENCLAW_NODE_DEVICE_COMMAND_BACKLIGHT,
    OPENCLAW_NODE_DEVICE_COMMAND_RESTART,
    OPENCLAW_NODE_DEVICE_COMMAND_BUTTON_STATUS,
    OPENCLAW_NODE_DEVICE_COMMAND_AUDIO_VOLUME,
};

const char *const *openclaw_node_device_commands(size_t *count)
{
    if (count) *count = sizeof(s_commands) / sizeof(s_commands[0]);
    return s_commands;
}

static esp_err_t write_info(char *out, size_t size)
{
    uint8_t public_key[OPENCLAW_NODE_ED25519_PUBLIC_KEY_LEN];
    char id[OPENCLAW_NODE_ED25519_PUBLIC_KEY_LEN * 2 + 1];
    esp_chip_info_t chip = {0};
    uint8_t mac[6] = {0};
    if (openclaw_node_identity_get_public_key(public_key) != ESP_OK ||
        openclaw_node_identity_get_id(id, sizeof(id)) != ESP_OK) return ESP_ERR_INVALID_STATE;
    esp_chip_info(&chip);
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    int n = snprintf(out, size,
                     "{\"command\":\"device.info\",\"deviceId\":\"%s\","
                     "\"model\":\"M5Stack StickS3\",\"mcu\":\"ESP32-S3\","
                     "\"chipRevision\":%d,\"cores\":%d,\"flashMB\":8,\"psramMB\":8,"
                     "\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
                     "\"firmware\":\"ESP-OpenClaw\",\"firmwareVersion\":\"0.1.0\"}",
                     id, chip.revision, chip.cores, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return n < 0 || (size_t)n >= size ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

static esp_err_t write_status(char *out, size_t size)
{
    int n = snprintf(out, size,
                     "{\"command\":\"device.status\",\"uptimeMs\":%llu,"
                     "\"freeHeap\":%u,\"minFreeHeap\":%u,\"freePsram\":%u,"
                     "\"psramAvailable\":%s}",
                     (unsigned long long)(esp_timer_get_time() / 1000ULL),
                     (unsigned)esp_get_free_heap_size(),
                     (unsigned)esp_get_minimum_free_heap_size(),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     esp_psram_is_initialized() ? "true" : "false");
    return n < 0 || (size_t)n >= size ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

static esp_err_t write_network(char *out, size_t size)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {0};
    char ip_text[16] = "0.0.0.0";
    char gateway_text[16] = "0.0.0.0";
    bool connected = false;
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        esp_ip4addr_ntoa(&ip.ip, ip_text, sizeof(ip_text));
        esp_ip4addr_ntoa(&ip.gw, gateway_text, sizeof(gateway_text));
        connected = ip.ip.addr != 0;
    }
    int n = snprintf(out, size,
                     "{\"command\":\"device.network\",\"connected\":%s,"
                     "\"ip\":\"%s\",\"gateway\":\"%s\"}",
                     connected ? "true" : "false", ip_text, gateway_text);
    return n < 0 || (size_t)n >= size ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

static esp_err_t set_backlight(const char *params_json, char *out, size_t size)
{
    cJSON *params = cJSON_Parse(params_json ? params_json : "{}");
    cJSON *level = params ? cJSON_GetObjectItem(params, "level") : NULL;
    void *handle_ptr = NULL;

    periph_ledc_config_t *ledc_cfg = NULL;
    periph_ledc_handle_t *ledc_handle = NULL;
    esp_err_t err = ESP_OK;
    if (!cJSON_IsNumber(level) || level->valuedouble < 0 || level->valuedouble > 100 ||
        level->valuedouble != (int)level->valuedouble) {
        cJSON_Delete(params);
        return ESP_ERR_INVALID_ARG;
    }
    err = esp_board_manager_get_periph_handle("ledc_backlight", &handle_ptr);
    if (err == ESP_OK) {
        ledc_handle = (periph_ledc_handle_t *)handle_ptr;
        err = esp_board_periph_get_config("ledc_backlight", (void **)&ledc_cfg);
    }
    if (err == ESP_OK) {
        uint32_t max_duty = (1U << (uint32_t)ledc_cfg->duty_resolution) - 1U;
        uint32_t duty = ((uint32_t)level->valuedouble * max_duty) / 100U;
        err = ledc_set_duty(ledc_handle->speed_mode, ledc_handle->channel, duty);
        if (err == ESP_OK) err = ledc_update_duty(ledc_handle->speed_mode, ledc_handle->channel);
    }
    cJSON_Delete(params);
    if (err != ESP_OK) return err;
    int n = snprintf(out, size, "{\"command\":\"device.backlight\",\"level\":%d}", (int)level->valuedouble);
    return n < 0 || (size_t)n >= size ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

static esp_err_t button_status(char *out, size_t size)
{
    int key1 = gpio_get_level(GPIO_NUM_11);
    int key2 = gpio_get_level(GPIO_NUM_12);
    int n = snprintf(out, size,
                     "{\"command\":\"device.button.status\",\"button1\":{\"gpio\":11,\"pressed\":%s},"
                     "\"button2\":{\"gpio\":12,\"pressed\":%s}}",
                     key1 == 0 ? "true" : "false", key2 == 0 ? "true" : "false");
    return n < 0 || (size_t)n >= size ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

static esp_err_t audio_volume(const char *params_json, char *out, size_t size)
{
    cJSON *params = cJSON_Parse(params_json ? params_json : "{}");
    cJSON *volume = params ? cJSON_GetObjectItem(params, "volume") : NULL;
    void *device = NULL;
    int value;
    esp_err_t err = ESP_OK;
    if (!cJSON_IsNumber(volume) || volume->valuedouble < 0 || volume->valuedouble > 100 ||
        volume->valuedouble != (int)volume->valuedouble) {
        cJSON_Delete(params);
        return ESP_ERR_INVALID_ARG;
    }
    value = (int)volume->valuedouble;
    err = esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_AUDIO_DAC, &device);
    if (err == ESP_OK && device) {
        esp_codec_dev_handle_t codec = *(esp_codec_dev_handle_t *)device;
        err = esp_codec_dev_set_out_vol(codec, value) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
    }
    cJSON_Delete(params);
    if (err != ESP_OK) return err;
    int n = snprintf(out, size, "{\"command\":\"audio.volume\",\"volume\":%d}", value);
    return n < 0 || (size_t)n >= size ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

esp_err_t openclaw_node_device_command(const char *command,
                                       const char *params_json,
                                       char *result_json,
                                       size_t result_size,
                                       void *user_ctx)
{
    (void)params_json;
    (void)user_ctx;
    if (!command || !result_json || result_size == 0) return ESP_ERR_INVALID_ARG;
    if (strcmp(command, OPENCLAW_NODE_DEVICE_COMMAND_INFO) == 0) return write_info(result_json, result_size);
    if (strcmp(command, OPENCLAW_NODE_DEVICE_COMMAND_STATUS) == 0) return write_status(result_json, result_size);
    if (strcmp(command, OPENCLAW_NODE_DEVICE_COMMAND_NETWORK) == 0) return write_network(result_json, result_size);
    if (strcmp(command, OPENCLAW_NODE_DEVICE_COMMAND_BACKLIGHT) == 0) return set_backlight(params_json, result_json, result_size);
    if (strcmp(command, OPENCLAW_NODE_DEVICE_COMMAND_RESTART) == 0) {
        if (xTaskCreate(restart_task, "node_restart", 2048, NULL, 5, NULL) != pdPASS) return ESP_ERR_NO_MEM;
        snprintf(result_json, result_size, "{\"command\":\"device.restart\",\"scheduled\":true}");
        return ESP_OK;
    }
    if (strcmp(command, OPENCLAW_NODE_DEVICE_COMMAND_BUTTON_STATUS) == 0) return button_status(result_json, result_size);
    if (strcmp(command, OPENCLAW_NODE_DEVICE_COMMAND_AUDIO_VOLUME) == 0) return audio_volume(params_json, result_json, result_size);
    return ESP_ERR_NOT_FOUND;
}
