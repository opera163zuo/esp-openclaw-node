/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*openclaw_node_command_cb_t)(const char *command,
                                                const char *params_json,
                                                char *result_json,
                                                size_t result_size,
                                                void *user_ctx);

/* The callback signs the exact UTF-8 v3 device-auth payload. The returned
 * signature must be unpadded base64url and fit in signature_out. */
typedef esp_err_t (*openclaw_node_sign_cb_t)(const char *payload,
                                             char *signature_out,
                                             size_t signature_size,
                                             void *user_ctx);

typedef struct {
    const char *gateway_url;       /* ws:// or wss:// Gateway endpoint */
    const char *gateway_token;     /* optional node token */
    const char *device_id;         /* stable Ed25519 public-key fingerprint */
    const char *public_key_b64url; /* raw 32-byte Ed25519 public key */
    const char *client_id;
    const char *client_version;
    const char *platform;
    const char *device_family;
    const char *const *commands;
    size_t command_count;
    openclaw_node_sign_cb_t sign_cb;
    openclaw_node_command_cb_t command_cb;
    void *user_ctx;
} openclaw_node_config_t;

/** Start the persistent OpenClaw Native Node WebSocket client. */
esp_err_t openclaw_node_start(const openclaw_node_config_t *config);

/** Stop the client and release its WebSocket resources. */
esp_err_t openclaw_node_stop(void);

/** Return true after the Gateway hello-ok has been received. */
bool openclaw_node_is_connected(void);

#ifdef __cplusplus
}
#endif
