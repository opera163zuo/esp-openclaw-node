/*
 * ESP-OpenClaw device identity storage and Ed25519 signing.
 * The private seed remains in NVS and is never returned to callers.
 */
#include "openclaw_node_identity.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_random.h"
#include "esp_log.h"
#include "monocypher-ed25519.h"
#include <stdio.h>
#include <string.h>

static const char s_b64url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static esp_err_t base64url_encode(const uint8_t *src, size_t src_len, char *out, size_t out_size)
{
    size_t max_len = ((src_len + 2) / 3) * 4;
    if (out_size < max_len) return ESP_ERR_INVALID_SIZE;
    size_t produced = 0;
    for (size_t i = 0; i < src_len; i += 3) {
        uint32_t v = (uint32_t)src[i] << 16;
        if (i + 1 < src_len) v |= (uint32_t)src[i + 1] << 8;
        if (i + 2 < src_len) v |= src[i + 2];
        out[produced++] = s_b64url[(v >> 18) & 0x3f];
        out[produced++] = s_b64url[(v >> 12) & 0x3f];
        if (i + 1 < src_len) out[produced++] = s_b64url[(v >> 6) & 0x3f];
        if (i + 2 < src_len) out[produced++] = s_b64url[v & 0x3f];
    }
    out[produced] = '\0';
    return ESP_OK;
}

#define TAG "openclaw_identity"
#define NVS_NAMESPACE "openclaw"
#define NVS_KEY "ed25519_seed"
#define SEED_LEN 32

static uint8_t s_seed[SEED_LEN];
static uint8_t s_secret_key[64];
static uint8_t s_public[OPENCLAW_NODE_ED25519_PUBLIC_KEY_LEN];
static bool s_ready;

static esp_err_t save_seed(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, NVS_KEY, s_seed, sizeof(s_seed));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t load_seed(bool *exists)
{
    nvs_handle_t h;
    size_t len = sizeof(s_seed);
    *exists = false;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_get_blob(h, NVS_KEY, s_seed, &len);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK || len != sizeof(s_seed)) return ESP_ERR_INVALID_SIZE;
    *exists = true;
    return ESP_OK;
}

esp_err_t openclaw_node_identity_init(void)
{
    if (s_ready) return ESP_OK;
    bool exists = false;
    esp_err_t err = load_seed(&exists);
    if (err != ESP_OK) return err;
    if (!exists) {
        esp_fill_random(s_seed, sizeof(s_seed));
        err = save_seed();
        if (err != ESP_OK) return err;
    }
    uint8_t seed_copy[SEED_LEN];
    memcpy(seed_copy, s_seed, sizeof(seed_copy));
    crypto_ed25519_key_pair(s_secret_key, s_public, seed_copy);
    crypto_wipe(seed_copy, sizeof(seed_copy));
    s_ready = true;
    return ESP_OK;
}

esp_err_t openclaw_node_identity_get_public_key(uint8_t out[OPENCLAW_NODE_ED25519_PUBLIC_KEY_LEN])
{
    if (!out || !s_ready) return ESP_ERR_INVALID_STATE;
    memcpy(out, s_public, sizeof(s_public));
    return ESP_OK;
}

esp_err_t openclaw_node_identity_get_id(char *out, size_t out_size)
{
    if (!out || out_size < sizeof(s_public) * 2 + 1 || !s_ready) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < sizeof(s_public); ++i) snprintf(out + i * 2, 3, "%02x", s_public[i]);
    return ESP_OK;
}

esp_err_t openclaw_node_identity_get_public_key_b64url(char *out, size_t out_size)
{
    if (!out || !s_ready) return ESP_ERR_INVALID_ARG;
    return base64url_encode(s_public, sizeof(s_public), out, out_size);
}

esp_err_t openclaw_node_identity_sign(const uint8_t *payload, size_t payload_len,
                                      uint8_t signature[OPENCLAW_NODE_ED25519_SIGNATURE_LEN])
{
    if (!payload || !signature || !s_ready) return ESP_ERR_INVALID_ARG;
    crypto_ed25519_sign(signature, s_secret_key, payload, payload_len);
    return ESP_OK;
}

esp_err_t openclaw_node_identity_sign_b64url(const char *payload,
                                             char *out, size_t out_size)
{
    uint8_t signature[OPENCLAW_NODE_ED25519_SIGNATURE_LEN];
    if (!payload || !out || out_size < 87) return ESP_ERR_INVALID_ARG;
    esp_err_t err = openclaw_node_identity_sign((const uint8_t *)payload,
                                                 strlen(payload), signature);
    if (err != ESP_OK) return err;
    err = base64url_encode(signature, sizeof(signature), out, out_size);
    if (err == ESP_OK) out[86] = '\0';
    return err;
}
