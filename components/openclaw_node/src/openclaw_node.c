/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * OpenClaw Native Node transport skeleton. This component deliberately owns
 * transport/framing only; board commands and key storage are injected by the
 * application. It is not an Agent and never sends prompts to an LLM.
 */
#include "openclaw_node.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_event.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_check.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#define TAG "openclaw_node"
#define PROTOCOL_VERSION 4
#define RX_BUFFER_SIZE 8192
#define MAX_FRAME_SIZE (64 * 1024)

static int utf8_invalid_offset(const char *data, size_t len)
{
    size_t i = 0;
    while (i < len) {
        uint8_t c = (uint8_t)data[i];
        size_t need = c < 0x80 ? 0 : (c >= 0xc2 && c <= 0xdf) ? 1 :
                      (c >= 0xe0 && c <= 0xef) ? 2 :
                      (c >= 0xf0 && c <= 0xf4) ? 3 : SIZE_MAX;
        if (need == SIZE_MAX || i + need >= len) return (int)i;
        for (size_t j = 1; j <= need; ++j) {
            if (((uint8_t)data[i + j] & 0xc0) != 0x80) return (int)(i + j);
        }
        if ((need == 2 && c == 0xe0 && (uint8_t)data[i + 1] < 0xa0) ||
            (need == 2 && c == 0xed && (uint8_t)data[i + 1] >= 0xa0) ||
            (need == 3 && c == 0xf0 && (uint8_t)data[i + 1] < 0x90) ||
            (need == 3 && c == 0xf4 && (uint8_t)data[i + 1] >= 0x90)) return (int)i;
        i += need + 1;
    }
    return -1;
}

typedef struct {
    openclaw_node_config_t cfg;
    esp_websocket_client_handle_t ws;
    char rx_buffer[RX_BUFFER_SIZE];
    size_t rx_length;
    char nonce[256];
    uint64_t challenge_ts;
    bool challenged;
    bool connected;
} node_state_t;

static node_state_t s_node;

static bool command_declared(const char *command)
{
    for (size_t i = 0; i < s_node.cfg.command_count; ++i) {
        if (strcmp(command, s_node.cfg.commands[i]) == 0) return true;
    }
    return false;
}

static esp_err_t send_json(cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    if (!text) return ESP_ERR_NO_MEM;
    size_t len = strlen(text);
    int invalid = utf8_invalid_offset(text, len);
    if (invalid >= 0) {
        ESP_LOGE(TAG, "TX JSON contains invalid UTF-8 at byte %d", invalid);
        int start = invalid > 16 ? invalid - 16 : 0;
        int count = (int)len - start;
        if (count > 48) count = 48;
        char hex[145];
        int pos = 0;
        for (int i = 0; i < count && pos < (int)sizeof(hex) - 4; ++i) {
            pos += snprintf(hex + pos, sizeof(hex) - (size_t)pos, "%02x%s",
                            (unsigned char)text[start + i], i + 1 == count ? "" : " ");
        }
        ESP_LOGE(TAG, "TX bytes around invalid UTF-8: %s", hex);
    }
    esp_err_t err = (len > MAX_FRAME_SIZE) ? ESP_ERR_INVALID_SIZE :
        (esp_websocket_client_send_text(s_node.ws, text, (int)len, pdMS_TO_TICKS(5000)) < 0
         ? ESP_FAIL : ESP_OK);
    ESP_LOGI(TAG, "TX JSON length=%u result=%s", (unsigned)len,
             err == ESP_OK ? "ok" : "failed");
    cJSON_free(text);
    return err;
}

static esp_err_t send_invoke_result(const char *id, bool ok,
                                    const char *payload_json, const char *error_message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *params = cJSON_CreateObject();
    if (!root || !params) {
        cJSON_Delete(root);
        cJSON_Delete(params);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", "req");
    /* Native node commands are Gateway events; reply through the node RPC. */
    cJSON_AddStringToObject(root, "id", id);
    cJSON_AddStringToObject(root, "method", "node.invoke.result");
    cJSON_AddStringToObject(params, "id", id);
    cJSON_AddStringToObject(params, "nodeId", s_node.cfg.device_id);
    cJSON_AddBoolToObject(params, "ok", ok);
    if (ok) cJSON_AddStringToObject(params, "payloadJSON", payload_json ? payload_json : "{}");
    else {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "code", "INVALID_REQUEST");
        cJSON_AddStringToObject(error, "message", error_message ?: "command failed");
        cJSON_AddItemToObject(params, "error", error);
    }
    cJSON_AddItemToObject(root, "params", params);
    esp_err_t err = send_json(root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t send_connect(void)
{
    if (!s_node.challenged || !s_node.cfg.sign_cb) return ESP_ERR_INVALID_STATE;
    char payload[1024];
    char signature[90];
    uint64_t signed_at_ms = s_node.challenge_ts ? s_node.challenge_ts : (uint64_t)esp_timer_get_time() / 1000ULL;
    int n = snprintf(payload, sizeof(payload), "v3|%s|%s|node|node||%llu|%s|%s|%s|%s",
                     s_node.cfg.device_id, s_node.cfg.client_id,
                     (unsigned long long)signed_at_ms, s_node.cfg.gateway_token ? s_node.cfg.gateway_token : "",
                     s_node.nonce, s_node.cfg.platform ? s_node.cfg.platform : "esp32",
                     s_node.cfg.device_family ? s_node.cfg.device_family : "esp-openclaw");
    if (n < 0 || (size_t)n >= sizeof(payload)) return ESP_ERR_INVALID_SIZE;
    ESP_RETURN_ON_ERROR(s_node.cfg.sign_cb(payload, signature, sizeof(signature), s_node.cfg.user_ctx), TAG, "sign");

    cJSON *root = cJSON_CreateObject();
    cJSON *params = cJSON_CreateObject();
    cJSON *client = cJSON_CreateObject();
    cJSON *device = cJSON_CreateObject();
    if (!root || !params || !client || !device) {
        cJSON_Delete(root);
        cJSON_Delete(params);
        cJSON_Delete(client);
        cJSON_Delete(device);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", "req");
    cJSON_AddStringToObject(root, "id", "connect-1");
    cJSON_AddStringToObject(root, "method", "connect");
    cJSON_AddNumberToObject(params, "minProtocol", 3);
    cJSON_AddNumberToObject(params, "maxProtocol", PROTOCOL_VERSION);
    cJSON_AddStringToObject(client, "id", s_node.cfg.client_id);
    cJSON_AddStringToObject(client, "version", s_node.cfg.client_version ?: "0.1.0");
    cJSON_AddStringToObject(client, "platform", s_node.cfg.platform ?: "esp32");
    cJSON_AddStringToObject(client, "mode", "node");
    cJSON_AddStringToObject(client, "deviceFamily", s_node.cfg.device_family ?: "m5stack-sticks3");
    cJSON_AddItemToObject(params, "client", client);
    cJSON_AddStringToObject(params, "role", "node");
    cJSON_AddStringToObject(params, "locale", "en-US");
    cJSON *commands = cJSON_AddArrayToObject(params, "commands");
    for (size_t i = 0; i < s_node.cfg.command_count; ++i) cJSON_AddItemToArray(commands, cJSON_CreateString(s_node.cfg.commands[i]));
    cJSON_AddObjectToObject(params, "permissions");
    if (s_node.cfg.gateway_token) { cJSON *auth = cJSON_AddObjectToObject(params, "auth"); cJSON_AddStringToObject(auth, "token", s_node.cfg.gateway_token); }
    cJSON_AddStringToObject(params, "locale", "en-US");
    cJSON_AddStringToObject(params, "userAgent", "esp-openclaw/0.1.0");
    cJSON_AddStringToObject(device, "id", s_node.cfg.device_id);
    cJSON_AddStringToObject(device, "publicKey", s_node.cfg.public_key_b64url);
    cJSON_AddStringToObject(device, "signature", signature);
    cJSON_AddNumberToObject(device, "signedAt", signed_at_ms);
    cJSON_AddStringToObject(device, "nonce", s_node.nonce);
    cJSON_AddItemToObject(params, "device", device);
    cJSON_AddItemToObject(root, "params", params);
    esp_err_t err = send_json(root);
    cJSON_Delete(root);
    return err;
}

static void handle_frame(const char *data, size_t len)
{
    char *copy = strndup(data, len);
    if (!copy) return;
    cJSON *root = cJSON_Parse(copy);
    if (!root) {
        const char *error = cJSON_GetErrorPtr();
        unsigned offset = (error && error >= copy) ? (unsigned)(error - copy) : 0;
        ESP_LOGE(TAG, "Gateway JSON parse failed at byte %u: %s", offset, copy);
        free(copy);
        return;
    }
    ESP_LOGI(TAG, "Gateway frame JSON parsed (%u bytes)", (unsigned)len);
    free(copy);
    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(type) && strcmp(type->valuestring, "event") == 0) {
        cJSON *event = cJSON_GetObjectItem(root, "event");
        cJSON *payload = cJSON_GetObjectItem(root, "payload");
        if (cJSON_IsString(event) && strcmp(event->valuestring, "connect.challenge") == 0 && payload) {
            cJSON *nonce = cJSON_GetObjectItem(payload, "nonce");
            cJSON *ts = cJSON_GetObjectItem(payload, "ts");
            ESP_LOGI(TAG, "Gateway challenge nonce_type=%d ts_type=%d", nonce ? nonce->type : -1, ts ? ts->type : -1);
            if (cJSON_IsString(nonce) && cJSON_IsNumber(ts) && ts->valuedouble >= 0 && strlen(nonce->valuestring) < sizeof(s_node.nonce)) {
                strlcpy(s_node.nonce, nonce->valuestring, sizeof(s_node.nonce)); s_node.challenge_ts = (uint64_t)ts->valuedouble; s_node.challenged = true; send_connect();
            } else ESP_LOGE(TAG, "Invalid Gateway challenge payload");
        } else if (cJSON_IsString(event) && strcmp(event->valuestring, "node.invoke.request") == 0 && payload) {
            cJSON *id = cJSON_GetObjectItem(payload, "id"); cJSON *command = cJSON_GetObjectItem(payload, "command"); cJSON *params = cJSON_GetObjectItem(payload, "paramsJSON");
            char result[2048] = "{}";
            if (!cJSON_IsString(id) || !cJSON_IsString(command) || !command_declared(command->valuestring) || !s_node.cfg.command_cb) { send_invoke_result(cJSON_IsString(id) ? id->valuestring : "invoke", false, NULL, "command not allowed"); }
            else { esp_err_t err = s_node.cfg.command_cb(command->valuestring, cJSON_IsString(params) ? params->valuestring : "{}", result, sizeof(result), s_node.cfg.user_ctx); send_invoke_result(id->valuestring, err == ESP_OK, result, "command failed"); }
        }
    } else if (cJSON_IsString(type) && strcmp(type->valuestring, "res") == 0) {
        cJSON *payload = cJSON_GetObjectItem(root, "payload");
        if (payload && cJSON_GetObjectItem(payload, "type") && strcmp(cJSON_GetObjectItem(payload, "type")->valuestring, "hello-ok") == 0) s_node.connected = true;
    }
    cJSON_Delete(root);
}

static void websocket_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg; (void)base;
    esp_websocket_event_data_t *event = event_data;
    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "WebSocket connected; waiting for Gateway challenge");
    } else if (event_id == WEBSOCKET_EVENT_DATA && event) {
        ESP_LOGI(TAG, "WebSocket data opcode=%d offset=%d data_len=%d payload_len=%d",
                 event->op_code, event->payload_offset, event->data_len, event->payload_len);
        if (event->op_code == WS_TRANSPORT_OPCODES_CLOSE) {
            ESP_LOGW(TAG, "Gateway close bytes: %02x %02x", event->data_len > 0 ? event->data_ptr[0] : 0,
                     event->data_len > 1 ? event->data_ptr[1] : 0);
            return;
        }
        if (event->op_code != WS_TRANSPORT_OPCODES_TEXT) return;
        if (event->payload_offset == 0) s_node.rx_length = 0;
        if (event->payload_len <= 0 || (size_t)event->payload_len >= sizeof(s_node.rx_buffer) ||
            event->payload_offset != (int)s_node.rx_length ||
            event->data_len < 0 || (size_t)event->data_len > sizeof(s_node.rx_buffer) - s_node.rx_length - 1) {
            ESP_LOGE(TAG, "Gateway frame exceeds RX buffer or has invalid fragmentation");
            s_node.rx_length = 0;
        } else {
            memcpy(s_node.rx_buffer + s_node.rx_length, event->data_ptr, event->data_len);
            s_node.rx_length += event->data_len;
            s_node.rx_buffer[s_node.rx_length] = '\0';
            if (s_node.rx_length == (size_t)event->payload_len) {
                handle_frame(s_node.rx_buffer, s_node.rx_length);
                s_node.rx_length = 0;
            }
        }
    } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED) s_node.connected = false;
}

esp_err_t openclaw_node_start(const openclaw_node_config_t *config)
{
    if (!config || !config->gateway_url || !config->device_id || !config->public_key_b64url || !config->client_id || !config->commands || !config->command_count || !config->sign_cb || !config->command_cb || s_node.ws) return ESP_ERR_INVALID_ARG;
    memset(&s_node, 0, sizeof(s_node)); s_node.cfg = *config;
    esp_websocket_client_config_t ws_cfg = {
        .uri = config->gateway_url,
        .network_timeout_ms = 10000,
        .reconnect_timeout_ms = 5000,
        .buffer_size = RX_BUFFER_SIZE,
        .task_stack = 8192,
        .subprotocol = "openclaw",
    };
    s_node.ws = esp_websocket_client_init(&ws_cfg); if (!s_node.ws) return ESP_ERR_NO_MEM;
    esp_websocket_register_events(s_node.ws, WEBSOCKET_EVENT_ANY, websocket_handler, NULL);
    return esp_websocket_client_start(s_node.ws);
}

esp_err_t openclaw_node_stop(void)
{
    if (!s_node.ws) return ESP_OK;
    esp_websocket_client_stop(s_node.ws); esp_websocket_client_destroy(s_node.ws); memset(&s_node, 0, sizeof(s_node)); return ESP_OK;
}

bool openclaw_node_is_connected(void) { return s_node.connected; }
