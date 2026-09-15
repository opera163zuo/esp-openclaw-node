/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the background SNTP client.
 *
 * The Gateway connection runs over TLS, so the wall clock has to be roughly
 * correct before the first handshake. This component owns that responsibility
 * on its own, so the device no longer needs the former Agent-side system
 * capability just to get the time.
 *
 * Safe to call before Wi-Fi is up: the worker waits for the interface to obtain
 * an address and then keeps retrying until the clock is plausible.
 *
 * @return ESP_OK when the worker was started or was already running.
 */
esp_err_t time_sync_start(void);

/** @brief Stop the worker and release the SNTP client. Safe when not started. */
void time_sync_stop(void);

/** @brief True once the wall clock has been set to something plausible. */
bool time_sync_is_valid(void);

#ifdef __cplusplus
}
#endif
