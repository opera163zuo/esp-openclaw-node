/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"

char *http_server_alloc_scratch_buffer(void)
{
    return heap_caps_malloc_prefer(HTTP_SERVER_SCRATCH_SIZE,
                                   2,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

bool http_server_require_auth(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();
    app_config_t *config = calloc(1, sizeof(*config));
    char header[256];
    char supplied[APP_CONFIG_STR_LEN];
    bool allowed = false;
    bool password_required = false;

    if (!config) {
        httpd_resp_send_500(req);
        return false;
    }
    if (ctx->services.load_config(config) != ESP_OK) {
        free(config);
        httpd_resp_send_500(req);
        return false;
    }

    /* No password configured: the portal stays open so that first-boot
     * provisioning cannot lock anyone out. */
    password_required = config->portal_password[0] != '\0';

    if (password_required &&
        httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) == ESP_OK &&
        strncmp(header, "Basic ", 6) == 0) {
        unsigned char decoded[APP_CONFIG_STR_LEN * 2];
        size_t decoded_len = 0;
        if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                                  (const unsigned char *)header + 6, strlen(header + 6)) == 0) {
            decoded[decoded_len] = '\0';
            /* The header carries "user:password"; the user name is ignored. */
            const char *colon = strchr((const char *)decoded, ':');
            strlcpy(supplied, colon ? colon + 1 : "", sizeof(supplied));
            allowed = strcmp(supplied, config->portal_password) == 0;
        }
    }
    free(config);

    if (password_required && !allowed) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP-OpenClaw\"");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
        httpd_resp_sendstr(req, "{\"error\":\"portal password required\"}");
        return false;
    }
    return true;
}

/* NOTE: this is the WebUI's own path check, separate from the one the Gateway
 * commands go through (cap_files_path_is_valid in components/claw_capabilities/
 * cap_files). The two are deliberately different shapes - cap_files resolves
 * absolute paths against several roots with per-root read-only flags, while this
 * one is confined to ctx->storage_base_path and only ever sees relative paths -
 * but the traversal rule they share ("no .. anywhere") must stay identical. If
 * you tighten one, tighten the other. */
bool http_server_path_is_safe(const char *path)
{
    return path && path[0] == '/' && strstr(path, "..") == NULL;
}

void http_server_url_decode_inplace(char *value)
{
    if (!value) {
        return;
    }

    char *src = value;
    char *dst = value;
    while (*src) {
        if (src[0] == '%' && src[1] && src[2]) {
            char hi = src[1];
            char lo = src[2];
            uint8_t decoded = 0;

            if (hi >= '0' && hi <= '9') {
                decoded = (uint8_t)(hi - '0') << 4;
            } else if (hi >= 'A' && hi <= 'F') {
                decoded = (uint8_t)(hi - 'A' + 10) << 4;
            } else if (hi >= 'a' && hi <= 'f') {
                decoded = (uint8_t)(hi - 'a' + 10) << 4;
            } else {
                *dst++ = *src++;
                continue;
            }

            if (lo >= '0' && lo <= '9') {
                decoded |= (uint8_t)(lo - '0');
            } else if (lo >= 'A' && lo <= 'F') {
                decoded |= (uint8_t)(lo - 'A' + 10);
            } else if (lo >= 'a' && lo <= 'f') {
                decoded |= (uint8_t)(lo - 'a' + 10);
            } else {
                *dst++ = *src++;
                continue;
            }

            *dst++ = (char)decoded;
            src += 3;
            continue;
        }

        if (*src == '+') {
            *dst++ = ' ';
            src++;
            continue;
        }

        *dst++ = *src++;
    }

    *dst = '\0';
}

esp_err_t http_server_query_get(httpd_req_t *req, const char *key, char *value, size_t value_size)
{
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    char *query = calloc(1, query_len + 1);
    if (!query) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = httpd_req_get_url_query_str(req, query, query_len + 1);
    if (err == ESP_OK) {
        err = httpd_query_key_value(query, key, value, value_size);
        if (err == ESP_OK) {
            http_server_url_decode_inplace(value);
        }
    }

    free(query);
    return err;
}

esp_err_t http_server_resolve_storage_path(const char *relative_path, char *full_path, size_t full_path_size)
{
    http_server_ctx_t *ctx = http_server_ctx();

    if (!http_server_path_is_safe(relative_path)) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(full_path, full_path_size, "%s%s", ctx->storage_base_path, relative_path);
    return (written <= 0 || (size_t)written >= full_path_size) ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

bool http_server_build_child_relative_path(const char *base_path,
                                           const char *entry_name,
                                           char *out_path,
                                           size_t out_path_size)
{
    if (!base_path || !entry_name || !out_path || out_path_size == 0) {
        return false;
    }

    if (strcmp(base_path, "/") == 0) {
        if (strlcpy(out_path, "/", out_path_size) >= out_path_size) {
            return false;
        }
    } else if (strlcpy(out_path, base_path, out_path_size) >= out_path_size) {
        return false;
    }

    if (strcmp(base_path, "/") != 0 && strlcat(out_path, "/", out_path_size) >= out_path_size) {
        return false;
    }

    return strlcat(out_path, entry_name, out_path_size) < out_path_size;
}
