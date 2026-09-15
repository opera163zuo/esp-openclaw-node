/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "system_ui_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t system_ui_start(const system_ui_config_t *config);
void system_ui_stop(void);
bool system_ui_is_started(void);

esp_err_t system_ui_show_home(void);
esp_err_t system_ui_show_text(const char *text);
esp_err_t system_ui_fullscreen_enter(void);
esp_err_t system_ui_fullscreen_text(const char *text, const char *orientation);
esp_err_t system_ui_fullscreen_clear(void);
esp_err_t system_ui_fullscreen_exit(void);
esp_err_t system_ui_reload_home(void);
esp_err_t system_ui_set_callbacks(const system_ui_callbacks_t *callbacks, void *user_ctx);
esp_err_t system_ui_update_network(const system_ui_network_state_t *state);
esp_err_t system_ui_set_activity(bool active);
esp_err_t system_ui_show_task_panel(bool visible);
esp_err_t system_ui_refresh_tasks(void);
void system_ui_click_feedback(void);

/**
 * @brief Count the characters in @p text that the notice font cannot draw.
 *
 * Text is accepted and queued asynchronously, so the command layer otherwise
 * has no way to tell "this rendered" from "this rendered as missing-glyph
 * boxes". This reports the latter up front, against the very font the notice
 * layer draws with.
 *
 * Control characters are ignored: they drive layout rather than glyph lookup.
 * Both outputs are optional.
 *
 * @param text         Text to check, normally the same string passed to
 *                     @ref system_ui_show_text.
 * @param out_missing  Receives the number of characters without a glyph.
 * @param out_first    Receives the first such code point, or 0 when there is none.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for NULL text, or
 *         ESP_ERR_INVALID_STATE before the UI has started (font not loaded yet).
 */
esp_err_t system_ui_text_coverage(const char *text, size_t *out_missing, uint32_t *out_first);

#ifdef __cplusplus
}
#endif
