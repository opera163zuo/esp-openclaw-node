#include "openclaw_node_device.h"
#include "openclaw_node_identity.h"
#include "esp_chip_info.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_netif.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>

static const char *s_commands[] = {
    OPENCLAW_NODE_DEVICE_COMMAND_INFO,
    OPENCLAW_NODE_DEVICE_COMMAND_STATUS,
    OPENCLAW_NODE_DEVICE_COMMAND_NETWORK,
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
    return ESP_ERR_NOT_FOUND;
}
