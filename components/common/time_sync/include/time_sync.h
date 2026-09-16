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

/**
 * @brief Wait, up to @p timeout_ms, for the wall clock to become plausible.
 *
 * A TLS client validates the server certificate against the wall clock, so a
 * caller that is about to open a `wss://` connection wants the clock set before
 * its first handshake - otherwise that attempt is thrown away on a 1970
 * timestamp. The background worker keeps retrying, so waiting here only helps;
 * whatever is left over is covered by the caller's own reconnect loop.
 *
 * Returns immediately when the clock is already plausible.
 *
 * @return ESP_OK once the clock is plausible, ESP_ERR_TIMEOUT if it is still
 *         unset when the budget runs out.
 */
esp_err_t time_sync_wait_valid(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
