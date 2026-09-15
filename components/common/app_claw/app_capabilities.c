/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_capabilities.h"

#include "app_lua_modules.h"
#include "cap_files.h"
#include "cap_lua.h"
#include "claw_cap.h"
#include "claw_paths.h"
#include "esp_check.h"
#include "esp_log.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app_capabilities";
#define APP_CAP_EXTERNAL_GROUP_INITIAL_CAPACITY 4

typedef struct {
    const char *group_id;
    const char *display_name;
    const char *label;
    app_capability_prepare_fn prepare;
    app_capability_register_fn reg;
} capability_entry_t;

static app_capability_external_group_t *s_external_groups;
static size_t s_external_group_count;
static size_t s_external_group_capacity;

static bool csv_empty(const char *value)
{
    if (!value) return true;
    while (*value) {
        if (!isspace((unsigned char)*value) && *value != ',') return false;
        ++value;
    }
    return true;
}

static bool csv_contains(const char *csv, const char *token)
{
    char item[64];
    size_t used = 0;

    if (csv_empty(csv) || !token || !token[0]) return false;
    while (*csv) {
        if (*csv == ',') {
            item[used] = '\0';
            if (strcmp(item, token) == 0) return true;
            used = 0;
        } else if (!isspace((unsigned char)*csv) && used + 1 < sizeof(item)) {
            item[used++] = *csv;
        }
        ++csv;
    }
    item[used] = '\0';
    return strcmp(item, token) == 0;
}

static bool group_enabled(const char *configured, const capability_entry_t *entry)
{
    return csv_empty(configured) || csv_contains(configured, entry->group_id);
}

static esp_err_t register_files(const app_claw_config_t *config,
                                const app_claw_storage_paths_t *paths)
{
    (void)config;
    (void)paths;
    return cap_files_register_group();
}

static esp_err_t prepare_lua(const app_claw_config_t *config,
                             const app_claw_storage_paths_t *paths)
{
    const char *system_root = claw_paths_get(CLAW_PATH_SYSTEM);
    char package_path[APP_CLAW_PATH_LEN * 2];

    ESP_RETURN_ON_FALSE(system_root, ESP_ERR_INVALID_STATE, TAG, "CLAW_PATH_SYSTEM not configured");
    snprintf(package_path, sizeof(package_path), "%s/scripts/builtin", system_root);
    ESP_RETURN_ON_ERROR(cap_lua_add_package_path_dir(package_path), TAG, "Failed to add Lua package path");
    snprintf(package_path, sizeof(package_path), "%s/scripts/builtin/lib", system_root);
    ESP_RETURN_ON_ERROR(cap_lua_add_package_path_dir(package_path), TAG, "Failed to add Lua library path");
    return app_lua_modules_register(config, paths->fatfs_base_path);
}

static esp_err_t register_lua(const app_claw_config_t *config,
                              const app_claw_storage_paths_t *paths)
{
    (void)config;
    (void)paths;
    return cap_lua_register_group();
}

static const capability_entry_t s_builtin[] = {
#if CONFIG_APP_CLAW_CAP_FILES
    { "cap_files", "Files", "Register files cap", NULL, register_files },
#endif
#if CONFIG_APP_CLAW_CAP_LUA
    { "cap_lua", "Lua", "Register Lua cap", prepare_lua, register_lua },
#endif
};

esp_err_t app_capabilities_register_external_group(const app_capability_external_group_t *group)
{
    if (!group || !group->group_id || !group->reg) return ESP_ERR_INVALID_ARG;
    if (s_external_group_count == s_external_group_capacity) {
        size_t capacity = s_external_group_capacity ? s_external_group_capacity * 2 : APP_CAP_EXTERNAL_GROUP_INITIAL_CAPACITY;
        void *next = realloc(s_external_groups, capacity * sizeof(*s_external_groups));
        if (!next) return ESP_ERR_NO_MEM;
        s_external_groups = next;
        s_external_group_capacity = capacity;
    }
    s_external_groups[s_external_group_count++] = *group;
    return ESP_OK;
}

esp_err_t app_capabilities_init(const app_claw_config_t *config,
                                const app_claw_storage_paths_t *paths)
{
    capability_entry_t *entries;
    size_t builtin_count = sizeof(s_builtin) / sizeof(s_builtin[0]);
    size_t total = builtin_count + s_external_group_count;
    size_t i;
    esp_err_t err;

    if (!config || !paths) return ESP_ERR_INVALID_ARG;
    entries = calloc(total ? total : 1, sizeof(*entries));
    if (!entries) return ESP_ERR_NO_MEM;
    memcpy(entries, s_builtin, sizeof(s_builtin));
    for (i = 0; i < s_external_group_count; ++i) {
        entries[builtin_count + i] = (capability_entry_t) {
            .group_id = s_external_groups[i].group_id,
            .display_name = s_external_groups[i].display_name,
            .label = s_external_groups[i].display_name,
            .prepare = s_external_groups[i].prepare,
            .reg = s_external_groups[i].reg,
        };
    }

    err = claw_cap_init();
    if (err != ESP_OK) goto cleanup;
    for (i = 0; i < total; ++i) {
        if (!group_enabled(config->enabled_cap_groups, &entries[i])) {
            ESP_LOGI(TAG, "Skipping capability group: %s", entries[i].group_id);
            continue;
        }
        if (entries[i].prepare) {
            err = entries[i].prepare(config, paths);
            if (err != ESP_OK) goto cleanup;
        }
        err = entries[i].reg(config, paths);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "%s failed: %s", entries[i].label, esp_err_to_name(err));
            goto cleanup;
        }
    }
    err = claw_cap_start_all();

cleanup:
    free(entries);
    return err;
}

esp_err_t app_capabilities_get_compiled_groups(const app_capability_group_info_t **groups,
                                               size_t *count)
{
    static app_capability_group_info_t *infos;
    static size_t info_count;
    size_t builtin_count = sizeof(s_builtin) / sizeof(s_builtin[0]);
    size_t total = builtin_count + s_external_group_count;
    size_t i;

    if (!groups || !count) return ESP_ERR_INVALID_ARG;
    free(infos);
    infos = calloc(total ? total : 1, sizeof(*infos));
    if (!infos) return ESP_ERR_NO_MEM;
    info_count = total;
    for (i = 0; i < builtin_count; ++i) {
        infos[i] = (app_capability_group_info_t) {
            .group_id = s_builtin[i].group_id,
            .display_name = s_builtin[i].display_name,
        };
    }
    for (i = 0; i < s_external_group_count; ++i) {
        infos[builtin_count + i] = (app_capability_group_info_t) {
            .group_id = s_external_groups[i].group_id,
            .display_name = s_external_groups[i].display_name,
        };
    }
    *groups = infos;
    *count = info_count;
    return ESP_OK;
}
