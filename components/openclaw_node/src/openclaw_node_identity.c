/*
 * ESP-OpenClaw device identity storage and Ed25519 signing.
 * The private seed remains in NVS and is never returned to callers.
 */
#include "openclaw_node_identity.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_random.h"
#include "esp_log.h"
#include "psa/crypto.h"
#include <stdio.h>
#include <string.h>

#define TAG "openclaw_identity"
#define NVS_NAMESPACE "openclaw"
#define NVS_KEY "ed25519_seed"
#define SEED_LEN 32

static uint8_t s_seed[SEED_LEN];
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

static esp_err_t derive_public_key(void)
{
    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t key = 0;
    size_t public_len = 0;
    psa_status_t st;
    psa_set_key_type(&attrs, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_TWISTED_EDWARDS));
    psa_set_key_bits(&attrs, 255);
    psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attrs, PSA_ALG_PURE_EDDSA);
    st = psa_import_key(&attrs, s_seed, sizeof(s_seed), &key);
    psa_reset_key_attributes(&attrs);
    if (st != PSA_SUCCESS) return ESP_ERR_NOT_SUPPORTED;
    st = psa_export_public_key(key, s_public, sizeof(s_public), &public_len);
    psa_destroy_key(key);
    if (st != PSA_SUCCESS || public_len != sizeof(s_public)) return ESP_ERR_NOT_SUPPORTED;
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
    if (psa_crypto_init() != PSA_SUCCESS) return ESP_ERR_NOT_SUPPORTED;
    err = derive_public_key();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ed25519 is unavailable in this ESP-IDF crypto configuration");
        return err;
    }
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

esp_err_t openclaw_node_identity_sign(const uint8_t *payload, size_t payload_len,
                                      uint8_t signature[OPENCLAW_NODE_ED25519_SIGNATURE_LEN])
{
    if (!payload || !signature || !s_ready) return ESP_ERR_INVALID_ARG;
    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t key = 0;
    size_t sig_len = 0;
    psa_set_key_type(&attrs, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_TWISTED_EDWARDS));
    psa_set_key_bits(&attrs, 255);
    psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attrs, PSA_ALG_PURE_EDDSA);
    psa_status_t st = psa_import_key(&attrs, s_seed, sizeof(s_seed), &key);
    psa_reset_key_attributes(&attrs);
    if (st == PSA_SUCCESS) st = psa_sign_message(key, PSA_ALG_PURE_EDDSA, payload, payload_len, signature, OPENCLAW_NODE_ED25519_SIGNATURE_LEN, &sig_len);
    if (key) psa_destroy_key(key);
    return (st == PSA_SUCCESS && sig_len == OPENCLAW_NODE_ED25519_SIGNATURE_LEN) ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}
