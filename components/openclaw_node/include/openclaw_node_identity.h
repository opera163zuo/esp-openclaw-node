/*
 * ESP-OpenClaw device identity storage and Ed25519 signing.
 * The private seed remains in NVS and is never returned to callers.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OPENCLAW_NODE_ED25519_PUBLIC_KEY_LEN 32
#define OPENCLAW_NODE_ED25519_SIGNATURE_LEN 64

/** Load an existing identity or create one on first boot. */
esp_err_t openclaw_node_identity_init(void);

/** Export the raw public key and a stable node id string. */
esp_err_t openclaw_node_identity_get_public_key(uint8_t out[OPENCLAW_NODE_ED25519_PUBLIC_KEY_LEN]);
esp_err_t openclaw_node_identity_get_id(char *out, size_t out_size);

/** Sign an exact UTF-8 payload with Ed25519 and return raw 64-byte signature. */
esp_err_t openclaw_node_identity_sign(const uint8_t *payload, size_t payload_len,
                                      uint8_t signature[OPENCLAW_NODE_ED25519_SIGNATURE_LEN]);

#ifdef __cplusplus
}
#endif
