/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_claw.h"
#include "app_claw_cli.h"
#include "app_capabilities.h"
#include "claw_paths.h"
#include "time_sync.h"
#if CONFIG_APP_CLAW_SYSTEM_UI_ENABLE
#include "system_ui.h"
#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#if CONFIG_APP_CLAW_CAP_LUA
#include "cap_lua.h"
#endif

static const char *TAG = "app_claw";

#define APP_CLAW_UI_OUTPUT_LEN 128
#define APP_CLAW_UI_JOBS_OUTPUT_LEN 4096
#define APP_CLAW_UI_JOB_STOP_WAIT_MS 50

#if CONFIG_APP_CLAW_SYSTEM_UI_ENABLE && CONFIG_APP_CLAW_CAP_LUA
static char *app_claw_split_job_field(char **cursor)
{
    char *field = NULL;
    char *sep = NULL;

    if (!cursor || !*cursor) {
        return NULL;
    }

    field = *cursor;
    sep = strstr(field, " | ");
    if (sep) {
        *sep = '\0';
        *cursor = sep + 3;
    } else {
        *cursor = NULL;
    }
    return field;
}

static const char *app_claw_job_field_value(const char *field, const char *prefix)
{
    size_t prefix_len = 0;

    if (!field || !prefix) {
        return "";
    }
    prefix_len = strlen(prefix);
    if (strncmp(field, prefix, prefix_len) == 0) {
        return field + prefix_len;
    }
    return field;
}

static bool app_claw_job_value_is_empty(const char *value)
{
    return !value || !value[0] || strcmp(value, "(unnamed)") == 0 || strcmp(value, "none") == 0;
}

static void app_claw_set_job_title(system_ui_task_item_t *item,
                                   const char *path,
                                   const char *name,
                                   const char *job_id)
{
    const char *title = NULL;
    const char *slash = NULL;
    size_t len = 0;

    if (!item) {
        return;
    }

    if (!app_claw_job_value_is_empty(path)) {
        slash = strrchr(path, '/');
        title = slash ? slash + 1 : path;
    }
    if (app_claw_job_value_is_empty(title) && !app_claw_job_value_is_empty(name)) {
        title = name;
    }
    if (app_claw_job_value_is_empty(title)) {
        title = job_id ? job_id : "";
    }

    len = strlen(title);
    if (len > 4 && strcmp(title + len - 4, ".lua") == 0) {
        len -= 4;
    }
    snprintf(item->title, sizeof(item->title), "%.*s", (int)len, title);
}

static size_t app_claw_parse_lua_jobs(char *text, system_ui_task_item_t *out, size_t max)
{
    size_t count = 0;
    char *line = text;

    if (!text || !out || max == 0 || strcmp(text, "(no Lua async jobs)") == 0) {
        return 0;
    }

    while (line && line[0] && count < max) {
        char *next_line = strchr(line, '\n');
        char *cursor = line;
        char *job_id = NULL;
        char *status = NULL;
        char *name_field = NULL;
        char *exclusive_field = NULL;
        char *runtime_field = NULL;
        char *path_field = NULL;
        const char *name = NULL;
        const char *exclusive = NULL;
        const char *runtime = NULL;
        const char *path = NULL;

        if (next_line) {
            *next_line = '\0';
            next_line++;
        }
        job_id = app_claw_split_job_field(&cursor);
        status = app_claw_split_job_field(&cursor);
        name_field = app_claw_split_job_field(&cursor);
        exclusive_field = app_claw_split_job_field(&cursor);
        runtime_field = app_claw_split_job_field(&cursor);
        path_field = cursor;

        if (!job_id || !status || !name_field || !runtime_field || !path_field) {
            line = next_line;
            continue;
        }

        name = app_claw_job_field_value(name_field, "name=");
        exclusive = app_claw_job_field_value(exclusive_field, "exclusive=");
        runtime = app_claw_job_field_value(runtime_field, "runtime=");
        path = app_claw_job_field_value(path_field, "path=");

        strlcpy(out[count].id, job_id, sizeof(out[count].id));
        app_claw_set_job_title(&out[count], path, name, job_id);
        strlcpy(out[count].status, status, sizeof(out[count].status));
        if (app_claw_job_value_is_empty(exclusive)) {
            snprintf(out[count].detail, sizeof(out[count].detail), "%s", runtime);
        } else {
            snprintf(out[count].detail, sizeof(out[count].detail),
                     "%s  exclusive=%s", runtime, exclusive);
        }
        count++;
        line = next_line;
    }
    return count;
}

static size_t app_claw_lua_jobs_provider(system_ui_task_item_t *out, size_t max, void *user_ctx)
{
    char *output = NULL;
    size_t count = 0;

    (void)user_ctx;
    if (!out || max == 0) {
        return 0;
    }

    output = calloc(1, APP_CLAW_UI_JOBS_OUTPUT_LEN);
    if (!output) {
        return 0;
    }

    if (cap_lua_list_jobs("running", output, APP_CLAW_UI_JOBS_OUTPUT_LEN) == ESP_OK) {
        count = app_claw_parse_lua_jobs(output, out, max);
    }
    free(output);
    return count;
}

static void app_claw_lua_job_event_cb(const cap_lua_job_event_t *event, void *user_ctx)
{
    (void)user_ctx;

    if (!event) {
        return;
    }
    switch (event->type) {
    case CAP_LUA_JOB_EVENT_CREATED:
    case CAP_LUA_JOB_EVENT_RUNNING:
    case CAP_LUA_JOB_EVENT_STOP_REQUESTED:
    case CAP_LUA_JOB_EVENT_TERMINAL:
        (void)system_ui_refresh_tasks();
        break;
    default:
        break;
    }
}

static esp_err_t app_claw_lua_job_stop_cb(const char *job_id, void *user_ctx)
{
    char *output = NULL;
    esp_err_t err;

    (void)user_ctx;
    if (!job_id || !job_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    output = calloc(1, APP_CLAW_UI_JOBS_OUTPUT_LEN);
    if (!output) {
        return ESP_ERR_NO_MEM;
    }
    err = cap_lua_stop_job(job_id, APP_CLAW_UI_JOB_STOP_WAIT_MS, output, APP_CLAW_UI_JOBS_OUTPUT_LEN);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "stop Lua job failed: job_id=%s err=%s output=%s",
                 job_id, esp_err_to_name(err), output);
        if (err == ESP_ERR_TIMEOUT) {
            err = ESP_OK;
        }
    } else {
        ESP_LOGI(TAG, "stop Lua job: job_id=%s output=%s", job_id, output);
    }
    free(output);
    return err;
}

static esp_err_t app_claw_lua_jobs_stop_all_cb(void *user_ctx)
{
    char *output = NULL;
    esp_err_t err;

    (void)user_ctx;

    output = calloc(1, APP_CLAW_UI_JOBS_OUTPUT_LEN);
    if (!output) {
        return ESP_ERR_NO_MEM;
    }
    err = cap_lua_stop_all_jobs(NULL, APP_CLAW_UI_JOB_STOP_WAIT_MS, output, APP_CLAW_UI_JOBS_OUTPUT_LEN);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "stop all Lua jobs failed: err=%s output=%s",
                 esp_err_to_name(err), output);
    } else {
        ESP_LOGI(TAG, "stop all Lua jobs: output=%s", output);
    }
    free(output);
    return err;
}

static esp_err_t app_claw_lua_display_exit_swipe_cb(void *user_ctx)
{
    char output[APP_CLAW_UI_OUTPUT_LEN] = {0};

    (void)user_ctx;
    esp_err_t err = cap_lua_stop_all_jobs("display", APP_CLAW_UI_JOB_STOP_WAIT_MS, output, sizeof(output));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "app exit swipe stop display jobs failed: err=%s output=%s", esp_err_to_name(err), output);
    } else {
        ESP_LOGI(TAG, "app exit swipe requested: output=%s", output);
    }
    return err;
}

#endif

static SemaphoreHandle_t s_config_lock;
static app_claw_config_t s_current_config;
static bool s_current_config_valid;

static esp_err_t app_claw_ensure_config_lock(void)
{
    if (!s_config_lock) {
        s_config_lock = xSemaphoreCreateMutex();
        if (!s_config_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static esp_err_t app_claw_store_current_config(const app_claw_config_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_ERROR(app_claw_ensure_config_lock(), TAG, "config lock unavailable");

    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    s_current_config = *config;
    s_current_config_valid = true;
    xSemaphoreGive(s_config_lock);
    return ESP_OK;
}

esp_err_t app_claw_get_config(app_claw_config_t *out_config)
{
    ESP_RETURN_ON_FALSE(out_config, ESP_ERR_INVALID_ARG, TAG, "out_config is NULL");
    ESP_RETURN_ON_ERROR(app_claw_ensure_config_lock(), TAG, "config lock unavailable");

    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    if (!s_current_config_valid) {
        xSemaphoreGive(s_config_lock);
        return ESP_ERR_INVALID_STATE;
    }
    *out_config = s_current_config;
    xSemaphoreGive(s_config_lock);
    return ESP_OK;
}

esp_err_t app_claw_ui_start(void)
{
#if CONFIG_APP_CLAW_SYSTEM_UI_ENABLE
    ESP_RETURN_ON_ERROR(system_ui_start(NULL), TAG, "start system UI failed");
#if CONFIG_APP_CLAW_CAP_LUA
    const system_ui_callbacks_t callbacks = {
        .get_tasks = app_claw_lua_jobs_provider,
        .on_stop_task = app_claw_lua_job_stop_cb,
        .on_stop_all_tasks = app_claw_lua_jobs_stop_all_cb,
        .on_app_exit_swipe = app_claw_lua_display_exit_swipe_cb,
    };
    ESP_RETURN_ON_ERROR(system_ui_set_callbacks(&callbacks, NULL), TAG, "set system UI callbacks failed");
    ESP_RETURN_ON_ERROR(cap_lua_register_job_event_cb(app_claw_lua_job_event_cb, NULL),
                        TAG, "register Lua job event callback failed");
#endif
    return ESP_OK;
#else
    return ESP_OK;
#endif
}

esp_err_t app_claw_set_network_status(bool sta_connected, const char *ap_ssid)
{
#if CONFIG_APP_CLAW_SYSTEM_UI_ENABLE
    const system_ui_network_state_t state = {
        .sta_connected = sta_connected,
        .ap_ssid = ap_ssid,
    };
    return system_ui_update_network(&state);
#else
    (void)sta_connected;
    (void)ap_ssid;
    return ESP_OK;
#endif
}

// Resolve the storage paths threaded through the capability framework from the
// logical homes registered in claw_paths. This is where app_claw owns the data
// layout (the subdirectory convention); main only decides the mount points.
static esp_err_t build_storage_paths(app_claw_storage_paths_t *paths)
{
    memset(paths, 0, sizeof(*paths));

    ESP_RETURN_ON_ERROR(claw_paths_join(CLAW_PATH_DATA, NULL, paths->fatfs_base_path, sizeof(paths->fatfs_base_path)),
                        TAG, "data home unavailable");
    ESP_RETURN_ON_ERROR(claw_paths_join(CLAW_PATH_DATA, "scripts", paths->lua_root_dir, sizeof(paths->lua_root_dir)),
                        TAG, "lua root path too long");

    return ESP_OK;
}

esp_err_t app_claw_start(const app_claw_config_t *config)
{
    app_claw_storage_paths_t paths;

    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(app_claw_store_current_config(config), TAG, "Failed to store Claw config");
    ESP_RETURN_ON_ERROR(build_storage_paths(&paths), TAG, "Failed to resolve storage paths");

    ESP_RETURN_ON_ERROR(app_capabilities_init(config, &paths), TAG, "Failed to init capabilities");
#if CONFIG_APP_CLAW_ENABLE_CLI
    ESP_RETURN_ON_ERROR(app_claw_cli_start(), TAG, "Failed to start CLI");
#endif

    /* The Gateway link runs over TLS, so the clock has to be roughly right
     * before the first handshake. Time sync used to ride on the Agent-side
     * system capability, which this build no longer has. */
    ESP_RETURN_ON_ERROR(time_sync_start(), TAG, "Failed to start time sync");

    ESP_LOGI(TAG, "App runtime started");

    return ESP_OK;
}

esp_err_t app_claw_update_config(const app_claw_config_t *config)
{
    ESP_RETURN_ON_ERROR(app_claw_store_current_config(config), TAG, "Failed to store Claw config");
    return ESP_OK;
}

esp_err_t app_claw_apply_config(const app_claw_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    return app_claw_update_config(config);
}
