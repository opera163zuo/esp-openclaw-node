/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stand-in for openclaw_node_device.c when CONFIG_OPENCLAW_NODE_DEVICE_COMMANDS
 * is off. The transport still has to export the two symbols so that callers
 * build and link against the same header; it just advertises no commands and
 * rejects every invoke.
 */
#include "openclaw_node_device.h"

const char *const *openclaw_node_device_commands(size_t *count)
{
    if (count) {
        *count = 0;
    }
    return NULL;
}

esp_err_t openclaw_node_device_command(const char *command,
                                       const char *params_json,
                                       char *result_json,
                                       size_t result_size,
                                       void *user_ctx)
{
    (void)command;
    (void)params_json;
    (void)result_json;
    (void)result_size;
    (void)user_ctx;
    return ESP_ERR_NOT_SUPPORTED;
}
