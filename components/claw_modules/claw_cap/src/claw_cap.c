/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_cap.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "claw_cap";

#define CLAW_CAP_DEFAULT_MAX_CAPABILITIES 4
#define CLAW_CAP_DEFAULT_MAX_GROUPS       4
#define CLAW_CAP_UNLOAD_POLL_MS          20

typedef struct {
    bool occupied;
    const claw_cap_group_t *group;
    claw_cap_state_t state;
    size_t *member_slots;
    size_t member_count;
    bool group_init_called;
} claw_cap_group_slot_t;

typedef struct {
    bool occupied;
    claw_cap_descriptor_t descriptor;
    size_t group_slot_index;
    claw_cap_state_t state;
    bool init_called;
    uint32_t active_calls;
} claw_cap_descriptor_slot_t;

typedef struct {
    bool initialized;
    bool started;
    SemaphoreHandle_t mutex;
    claw_cap_descriptor_slot_t *descriptor_slots;
    claw_cap_group_slot_t *group_slots;
    claw_cap_descriptor_t *descriptor_list_snapshot;
    claw_cap_group_info_t *group_list_snapshot;
    size_t descriptor_capacity;
    size_t group_capacity;
} claw_cap_runtime_t;

static EXT_RAM_BSS_ATTR claw_cap_runtime_t s_runtime = {0};

static size_t claw_cap_count_used_descriptor_slots_locked(void);
static size_t claw_cap_count_used_group_slots_locked(void);
static esp_err_t claw_cap_ensure_descriptor_capacity_locked(size_t additional_free_slots);
static esp_err_t claw_cap_ensure_group_capacity_locked(size_t additional_free_slots);

static void claw_cap_lock(void)
{
    xSemaphoreTake(s_runtime.mutex, portMAX_DELAY);
}

static void claw_cap_unlock(void)
{
    xSemaphoreGive(s_runtime.mutex);
}

const char *claw_cap_state_to_string(claw_cap_state_t state)
{
    switch (state) {
    case CLAW_CAP_STATE_REGISTERED:
        return "registered";
    case CLAW_CAP_STATE_STARTED:
        return "started";
    case CLAW_CAP_STATE_DISABLED:
        return "disabled";
    case CLAW_CAP_STATE_DRAINING:
        return "draining";
    case CLAW_CAP_STATE_UNLOADING:
        return "unloading";
    default:
        return "unknown";
    }
}

static bool claw_cap_descriptor_is_available(
    const claw_cap_descriptor_slot_t *slot)
{
    return slot && slot->occupied &&
           (slot->state == CLAW_CAP_STATE_REGISTERED ||
            slot->state == CLAW_CAP_STATE_STARTED);
}

static bool claw_cap_descriptor_is_listable(
    const claw_cap_descriptor_slot_t *slot)
{
    return claw_cap_descriptor_is_available(slot);
}

static esp_err_t claw_cap_validate_descriptor(
    const claw_cap_descriptor_t *descriptor)
{
    if (!descriptor || !descriptor->id || !descriptor->id[0] ||
            !descriptor->name || !descriptor->name[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    if ((descriptor->kind == CLAW_CAP_KIND_CALLABLE ||
            descriptor->kind == CLAW_CAP_KIND_HYBRID) && !descriptor->execute) {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static ssize_t claw_cap_find_group_slot_index_locked(const char *group_id)
{
    size_t i;

    if (!group_id || !group_id[0]) {
        return -1;
    }

    for (i = 0; i < s_runtime.group_capacity; i++) {
        if (s_runtime.group_slots[i].occupied &&
                s_runtime.group_slots[i].group &&
                s_runtime.group_slots[i].group->group_id &&
                strcmp(s_runtime.group_slots[i].group->group_id, group_id) == 0) {
            return (ssize_t)i;
        }
    }

    return -1;
}

static ssize_t claw_cap_find_free_group_slot_locked(void)
{
    size_t i;

    for (i = 0; i < s_runtime.group_capacity; i++) {
        if (!s_runtime.group_slots[i].occupied) {
            return (ssize_t)i;
        }
    }

    return -1;
}

static ssize_t claw_cap_find_descriptor_slot_index_locked(const char *id_or_name)
{
    size_t i;

    if (!id_or_name || !id_or_name[0]) {
        return -1;
    }

    for (i = 0; i < s_runtime.descriptor_capacity; i++) {
        claw_cap_descriptor_slot_t *slot = &s_runtime.descriptor_slots[i];

        if (!slot->occupied) {
            continue;
        }
        if (strcmp(slot->descriptor.id, id_or_name) == 0 ||
                strcmp(slot->descriptor.name, id_or_name) == 0) {
            return (ssize_t)i;
        }
    }

    return -1;
}

static ssize_t claw_cap_find_free_descriptor_slot_locked(void)
{
    size_t i;

    for (i = 0; i < s_runtime.descriptor_capacity; i++) {
        if (!s_runtime.descriptor_slots[i].occupied) {
            return (ssize_t)i;
        }
    }

    return -1;
}

static bool claw_cap_names_conflict_locked(
    const claw_cap_descriptor_t *descriptor)
{
    size_t i;

    for (i = 0; i < s_runtime.descriptor_capacity; i++) {
        claw_cap_descriptor_slot_t *slot = &s_runtime.descriptor_slots[i];

        if (!slot->occupied) {
            continue;
        }
        if (strcmp(slot->descriptor.id, descriptor->id) == 0 ||
                strcmp(slot->descriptor.name, descriptor->name) == 0) {
            return true;
        }
    }

    return false;
}

static size_t claw_cap_count_free_descriptor_slots_locked(void)
{
    size_t i;
    size_t count = 0;

    for (i = 0; i < s_runtime.descriptor_capacity; i++) {
        if (!s_runtime.descriptor_slots[i].occupied) {
            count++;
        }
    }

    return count;
}

static size_t claw_cap_count_used_descriptor_slots_locked(void)
{
    size_t i;
    size_t count = 0;

    for (i = 0; i < s_runtime.descriptor_capacity; i++) {
        if (s_runtime.descriptor_slots[i].occupied) {
            count++;
        }
    }

    return count;
}

static size_t claw_cap_count_used_group_slots_locked(void)
{
    size_t i;
    size_t count = 0;

    for (i = 0; i < s_runtime.group_capacity; i++) {
        if (s_runtime.group_slots[i].occupied) {
            count++;
        }
    }

    return count;
}

static esp_err_t claw_cap_ensure_descriptor_capacity_locked(size_t additional_free_slots)
{
    claw_cap_descriptor_slot_t *original_slots = s_runtime.descriptor_slots;
    claw_cap_descriptor_t *original_snapshot = s_runtime.descriptor_list_snapshot;
    claw_cap_descriptor_slot_t *new_slots = NULL;
    claw_cap_descriptor_t *new_snapshot = NULL;
    size_t required_capacity;
    size_t old_capacity;
    size_t new_capacity;

    if (additional_free_slots == 0 || claw_cap_count_free_descriptor_slots_locked() >= additional_free_slots) {
        return ESP_OK;
    }

    old_capacity = s_runtime.descriptor_capacity;
    required_capacity = claw_cap_count_used_descriptor_slots_locked() + additional_free_slots;
    new_capacity = s_runtime.descriptor_capacity ? s_runtime.descriptor_capacity : CLAW_CAP_DEFAULT_MAX_CAPABILITIES;
    // Descriptors grow with a small buffer instead of geometric jumps.
    if (new_capacity < required_capacity) {
        size_t growth_margin = required_capacity / 4;

        if (growth_margin < CLAW_CAP_DEFAULT_MAX_CAPABILITIES) {
            growth_margin = CLAW_CAP_DEFAULT_MAX_CAPABILITIES;
        }
        new_capacity = required_capacity + growth_margin;
    }

    new_slots = realloc(original_slots, new_capacity * sizeof(*new_slots));
    if (!new_slots) {
        return ESP_ERR_NO_MEM;
    }
    new_snapshot = realloc(original_snapshot, new_capacity * sizeof(*new_snapshot));
    if (!new_snapshot) {
        if (new_slots != original_slots) {
            void *restored = realloc(new_slots, old_capacity * sizeof(*new_slots));

            if (restored) {
                new_slots = restored;
            }
        }
        return ESP_ERR_NO_MEM;
    }

    if (new_capacity > old_capacity) {
        memset(&new_slots[old_capacity], 0, (new_capacity - old_capacity) * sizeof(*new_slots));
        memset(&new_snapshot[old_capacity], 0, (new_capacity - old_capacity) * sizeof(*new_snapshot));
    }
    s_runtime.descriptor_slots = new_slots;
    s_runtime.descriptor_list_snapshot = new_snapshot;
    s_runtime.descriptor_capacity = new_capacity;
    ESP_LOGD(TAG, "Expanded descriptor capacity to %u", (unsigned)new_capacity);
    return ESP_OK;
}

static esp_err_t claw_cap_ensure_group_capacity_locked(size_t additional_free_slots)
{
    claw_cap_group_slot_t *original_slots = s_runtime.group_slots;
    claw_cap_group_info_t *original_snapshot = s_runtime.group_list_snapshot;
    claw_cap_group_slot_t *new_slots = NULL;
    claw_cap_group_info_t *new_snapshot = NULL;
    size_t required_capacity;
    size_t old_capacity;
    size_t new_capacity;

    if (additional_free_slots == 0) {
        return ESP_OK;
    }
    if (s_runtime.group_capacity > 0) {
        size_t free_slots = s_runtime.group_capacity - claw_cap_count_used_group_slots_locked();

        if (free_slots >= additional_free_slots) {
            return ESP_OK;
        }
    }

    old_capacity = s_runtime.group_capacity;
    required_capacity = claw_cap_count_used_group_slots_locked() + additional_free_slots;
    new_capacity = s_runtime.group_capacity ? s_runtime.group_capacity : CLAW_CAP_DEFAULT_MAX_GROUPS;
    // Groups stay relatively few, so reserve only a small buffer above the exact need.
    if (new_capacity < required_capacity) {
        size_t growth_margin = required_capacity / 4;

        if (growth_margin < CLAW_CAP_DEFAULT_MAX_GROUPS) {
            growth_margin = CLAW_CAP_DEFAULT_MAX_GROUPS;
        }
        new_capacity = required_capacity + growth_margin;
    }

    new_slots = realloc(original_slots, new_capacity * sizeof(*new_slots));
    if (!new_slots) {
        return ESP_ERR_NO_MEM;
    }
    new_snapshot = realloc(original_snapshot, new_capacity * sizeof(*new_snapshot));
    if (!new_snapshot) {
        if (new_slots != original_slots) {
            void *restored = realloc(new_slots, old_capacity * sizeof(*new_slots));

            if (restored) {
                new_slots = restored;
            }
        }
        return ESP_ERR_NO_MEM;
    }

    if (new_capacity > old_capacity) {
        memset(&new_slots[old_capacity], 0, (new_capacity - old_capacity) * sizeof(*new_slots));
        memset(&new_snapshot[old_capacity], 0, (new_capacity - old_capacity) * sizeof(*new_snapshot));
    }
    s_runtime.group_slots = new_slots;
    s_runtime.group_list_snapshot = new_snapshot;
    s_runtime.group_capacity = new_capacity;
    ESP_LOGD(TAG, "Expanded group capacity to %u", (unsigned)new_capacity);
    return ESP_OK;
}

static esp_err_t claw_cap_validate_group_locked(const claw_cap_group_t *group)
{
    size_t i;
    size_t j;

    if (!group || !group->group_id || !group->group_id[0] ||
            !group->descriptors || group->descriptor_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (claw_cap_find_group_slot_index_locked(group->group_id) >= 0) {
        return ESP_ERR_INVALID_STATE;
    }

    for (i = 0; i < group->descriptor_count; i++) {
        esp_err_t err = claw_cap_validate_descriptor(&group->descriptors[i]);

        if (err != ESP_OK) {
            return err;
        }
        if (claw_cap_names_conflict_locked(&group->descriptors[i])) {
            return ESP_ERR_INVALID_STATE;
        }
        for (j = i + 1; j < group->descriptor_count; j++) {
            const claw_cap_descriptor_t *left = &group->descriptors[i];
            const claw_cap_descriptor_t *right = &group->descriptors[j];

            if (strcmp(left->id, right->id) == 0 ||
                    strcmp(left->name, right->name) == 0) {
                return ESP_ERR_INVALID_STATE;
            }
        }
    }

    return ESP_OK;
}

static esp_err_t claw_cap_start_group_locked(size_t group_slot_index)
{
    claw_cap_group_slot_t *group_slot = &s_runtime.group_slots[group_slot_index];
    size_t i;

    if (!group_slot->occupied || group_slot->state == CLAW_CAP_STATE_STARTED) {
        return ESP_OK;
    }
    if (group_slot->state == CLAW_CAP_STATE_DRAINING ||
            group_slot->state == CLAW_CAP_STATE_UNLOADING) {
        return ESP_ERR_INVALID_STATE;
    }

    group_slot->state = CLAW_CAP_STATE_STARTED;
    for (i = 0; i < group_slot->member_count; i++) {
        claw_cap_descriptor_slot_t *slot =
            &s_runtime.descriptor_slots[group_slot->member_slots[i]];

        slot->state = CLAW_CAP_STATE_STARTED;
    }
    return ESP_OK;
}

static esp_err_t claw_cap_start_group_callbacks(size_t group_slot_index)
{
    claw_cap_group_slot_t *group_slot = &s_runtime.group_slots[group_slot_index];
    size_t i;

    if (!group_slot->occupied) {
        return ESP_ERR_NOT_FOUND;
    }

    if (group_slot->group && group_slot->group->group_start) {
        esp_err_t err = group_slot->group->group_start();

        if (err != ESP_OK) {
            return err;
        }
    }

    for (i = 0; i < group_slot->member_count; i++) {
        claw_cap_descriptor_slot_t *slot =
            &s_runtime.descriptor_slots[group_slot->member_slots[i]];

        if (!slot->init_called && slot->descriptor.init) {
            esp_err_t err = slot->descriptor.init();

            if (err != ESP_OK) {
                return err;
            }
            slot->init_called = true;
        }
        if (slot->descriptor.start) {
            esp_err_t err = slot->descriptor.start();

            if (err != ESP_OK) {
                return err;
            }
        }
    }

    return ESP_OK;
}

static esp_err_t claw_cap_stop_group_callbacks(size_t group_slot_index)
{
    claw_cap_group_slot_t *group_slot = &s_runtime.group_slots[group_slot_index];
    esp_err_t first_err = ESP_OK;
    size_t i;

    if (!group_slot->occupied) {
        return ESP_ERR_NOT_FOUND;
    }

    for (i = group_slot->member_count; i > 0; i--) {
        claw_cap_descriptor_slot_t *slot =
            &s_runtime.descriptor_slots[group_slot->member_slots[i - 1]];

        if (slot->descriptor.stop) {
            esp_err_t err = slot->descriptor.stop();

            if (err != ESP_OK && first_err == ESP_OK) {
                first_err = err;
            }
        }
    }

    if (group_slot->group && group_slot->group->group_stop) {
        esp_err_t err = group_slot->group->group_stop();

        if (err != ESP_OK && first_err == ESP_OK) {
            first_err = err;
        }
    }

    return first_err;
}

static bool claw_cap_group_has_active_calls_locked(size_t group_slot_index)
{
    claw_cap_group_slot_t *group_slot = &s_runtime.group_slots[group_slot_index];
    size_t i;

    for (i = 0; i < group_slot->member_count; i++) {
        claw_cap_descriptor_slot_t *slot =
            &s_runtime.descriptor_slots[group_slot->member_slots[i]];

        if (slot->active_calls > 0) {
            return true;
        }
    }

    return false;
}

static void claw_cap_clear_group_slot_locked(size_t group_slot_index)
{
    claw_cap_group_slot_t *group_slot = &s_runtime.group_slots[group_slot_index];
    size_t i;

    for (i = 0; i < group_slot->member_count; i++) {
        size_t descriptor_slot_index = group_slot->member_slots[i];

        memset(&s_runtime.descriptor_slots[descriptor_slot_index], 0,
               sizeof(s_runtime.descriptor_slots[descriptor_slot_index]));
    }

    free(group_slot->member_slots);
    memset(group_slot, 0, sizeof(*group_slot));
}

static esp_err_t claw_cap_disable_group_locked(size_t group_slot_index)
{
    claw_cap_group_slot_t *group_slot = &s_runtime.group_slots[group_slot_index];
    size_t i;

    if (!group_slot->occupied) {
        return ESP_ERR_NOT_FOUND;
    }
    if (group_slot->state == CLAW_CAP_STATE_DISABLED) {
        return ESP_OK;
    }
    if (group_slot->state == CLAW_CAP_STATE_DRAINING ||
            group_slot->state == CLAW_CAP_STATE_UNLOADING) {
        return ESP_ERR_INVALID_STATE;
    }

    group_slot->state = CLAW_CAP_STATE_DISABLED;
    for (i = 0; i < group_slot->member_count; i++) {
        claw_cap_descriptor_slot_t *slot =
            &s_runtime.descriptor_slots[group_slot->member_slots[i]];

        slot->state = CLAW_CAP_STATE_DISABLED;
    }
    return ESP_OK;
}

static esp_err_t claw_cap_enable_group_locked(size_t group_slot_index)
{
    claw_cap_group_slot_t *group_slot = &s_runtime.group_slots[group_slot_index];
    size_t i;

    if (!group_slot->occupied) {
        return ESP_ERR_NOT_FOUND;
    }
    if (group_slot->state == CLAW_CAP_STATE_DRAINING ||
            group_slot->state == CLAW_CAP_STATE_UNLOADING) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_runtime.started) {
        group_slot->state = CLAW_CAP_STATE_STARTED;
        for (i = 0; i < group_slot->member_count; i++) {
            s_runtime.descriptor_slots[group_slot->member_slots[i]].state =
                CLAW_CAP_STATE_STARTED;
        }
    } else {
        group_slot->state = CLAW_CAP_STATE_REGISTERED;
        for (i = 0; i < group_slot->member_count; i++) {
            s_runtime.descriptor_slots[group_slot->member_slots[i]].state =
                CLAW_CAP_STATE_REGISTERED;
        }
    }
    return ESP_OK;
}

static esp_err_t claw_cap_register_group_locked(const claw_cap_group_t *group,
                                                size_t *out_group_slot_index)
{
    ssize_t group_slot_index;
    claw_cap_group_slot_t *group_slot;
    size_t i;
    esp_err_t err;

    err = claw_cap_ensure_group_capacity_locked(1);
    if (err != ESP_OK) {
        return err;
    }
    err = claw_cap_ensure_descriptor_capacity_locked(group->descriptor_count);
    if (err != ESP_OK) {
        return err;
    }

    group_slot_index = claw_cap_find_free_group_slot_locked();
    if (group_slot_index < 0) {
        return ESP_ERR_NO_MEM;
    }

    group_slot = &s_runtime.group_slots[group_slot_index];
    group_slot->member_slots = calloc(group->descriptor_count, sizeof(size_t));
    if (!group_slot->member_slots) {
        return ESP_ERR_NO_MEM;
    }

    group_slot->occupied = true;
    group_slot->group = group;
    group_slot->state = CLAW_CAP_STATE_REGISTERED;
    group_slot->member_count = group->descriptor_count;

    if (group->group_init) {
        esp_err_t err = group->group_init();

        if (err != ESP_OK) {
            free(group_slot->member_slots);
            memset(group_slot, 0, sizeof(*group_slot));
            return err;
        }
        group_slot->group_init_called = true;
    }

    for (i = 0; i < group->descriptor_count; i++) {
        ssize_t descriptor_slot_index = claw_cap_find_free_descriptor_slot_locked();
        claw_cap_descriptor_slot_t *slot;

        if (descriptor_slot_index < 0) {
            claw_cap_clear_group_slot_locked(group_slot_index);
            return ESP_ERR_NO_MEM;
        }

        slot = &s_runtime.descriptor_slots[descriptor_slot_index];
        slot->occupied = true;
        slot->descriptor = group->descriptors[i];
        slot->group_slot_index = (size_t)group_slot_index;
        slot->state = CLAW_CAP_STATE_REGISTERED;
        group_slot->member_slots[i] = (size_t)descriptor_slot_index;
    }

    if (out_group_slot_index) {
        *out_group_slot_index = (size_t)group_slot_index;
    }
    return ESP_OK;
}

esp_err_t claw_cap_init(void)
{
    if (s_runtime.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_runtime.mutex = xSemaphoreCreateMutex();
    if (!s_runtime.mutex) {
        if (s_runtime.mutex) {
            vSemaphoreDelete(s_runtime.mutex);
        }
        memset(&s_runtime, 0, sizeof(s_runtime));
        return ESP_ERR_NO_MEM;
    }

    s_runtime.initialized = true;
    ESP_LOGI(TAG, "Initialized runtime with dynamic capacity growth");
    return ESP_OK;
}

esp_err_t claw_cap_register(const claw_cap_descriptor_t *descriptor)
{
    claw_cap_group_t group = {
        .group_id = descriptor ? descriptor->id : NULL,
        .plugin_name = descriptor ? descriptor->name : NULL,
        .version = "1",
        .descriptors = descriptor,
        .descriptor_count = descriptor ? 1 : 0,
    };

    return claw_cap_register_group(&group);
}

esp_err_t claw_cap_register_group(const claw_cap_group_t *group)
{
    size_t group_slot_index = 0;
    esp_err_t err;

    if (!s_runtime.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    claw_cap_lock();
    err = claw_cap_validate_group_locked(group);
    if (err == ESP_OK) {
        err = claw_cap_register_group_locked(group, &group_slot_index);
    }
    claw_cap_unlock();
    if (err != ESP_OK) {
        return err;
    }

    if (s_runtime.started) {
        err = claw_cap_enable_group(group->group_id);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

esp_err_t claw_cap_start_all(void)
{
    size_t i;

    if (!s_runtime.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_runtime.started) {
        return ESP_OK;
    }

    s_runtime.started = true;
    for (i = 0; i < s_runtime.group_capacity; i++) {
        claw_cap_group_slot_t *group_slot = &s_runtime.group_slots[i];

        if (!group_slot->occupied || group_slot->state == CLAW_CAP_STATE_DISABLED) {
            continue;
        }

        if (claw_cap_enable_group(group_slot->group->group_id) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to start group %s", group_slot->group->group_id);
        }
    }

    return ESP_OK;
}

esp_err_t claw_cap_stop_all(void)
{
    esp_err_t first_err = ESP_OK;
    size_t i;

    if (!s_runtime.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    for (i = 0; i < s_runtime.group_capacity; i++) {
        claw_cap_group_slot_t *group_slot = &s_runtime.group_slots[i];

        if (!group_slot->occupied || group_slot->state != CLAW_CAP_STATE_STARTED) {
            continue;
        }
        if (claw_cap_disable_group(group_slot->group->group_id) != ESP_OK &&
                first_err == ESP_OK) {
            first_err = ESP_FAIL;
        }
    }

    s_runtime.started = false;
    return first_err;
}

esp_err_t claw_cap_enable_group(const char *group_id)
{
    ssize_t group_slot_index;
    esp_err_t err;

    if (!s_runtime.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!group_id || !group_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    claw_cap_lock();
    group_slot_index = claw_cap_find_group_slot_index_locked(group_id);
    if (group_slot_index < 0) {
        claw_cap_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    if (!s_runtime.started) {
        err = claw_cap_enable_group_locked((size_t)group_slot_index);
        claw_cap_unlock();
        return err;
    }
    if (s_runtime.group_slots[group_slot_index].state == CLAW_CAP_STATE_STARTED) {
        claw_cap_unlock();
        return ESP_OK;
    }
    err = claw_cap_enable_group_locked((size_t)group_slot_index);
    claw_cap_unlock();
    if (err != ESP_OK) {
        return err;
    }

    err = claw_cap_start_group_callbacks((size_t)group_slot_index);
    if (err != ESP_OK) {
        claw_cap_lock();
        claw_cap_disable_group_locked((size_t)group_slot_index);
        claw_cap_unlock();
        return err;
    }

    claw_cap_lock();
    err = claw_cap_start_group_locked((size_t)group_slot_index);
    claw_cap_unlock();
    return err;
}

esp_err_t claw_cap_disable_group(const char *group_id)
{
    ssize_t group_slot_index;
    esp_err_t err = ESP_OK;

    if (!s_runtime.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!group_id || !group_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    claw_cap_lock();
    group_slot_index = claw_cap_find_group_slot_index_locked(group_id);
    if (group_slot_index < 0) {
        claw_cap_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    if (s_runtime.group_slots[group_slot_index].state == CLAW_CAP_STATE_DISABLED) {
        claw_cap_unlock();
        return ESP_OK;
    }
    err = claw_cap_disable_group_locked((size_t)group_slot_index);
    claw_cap_unlock();
    if (err != ESP_OK) {
        return err;
    }

    err = claw_cap_stop_group_callbacks((size_t)group_slot_index);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Group stop failed for %s: %s", group_id, esp_err_to_name(err));
    }
    return ESP_OK;
}

esp_err_t claw_cap_unregister_group(const char *group_id, uint32_t timeout_ms)
{
    ssize_t group_slot_index;
    TickType_t deadline = xTaskGetTickCount() +
                          ((timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms));
    esp_err_t stop_err = ESP_OK;

    if (!s_runtime.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!group_id || !group_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    claw_cap_lock();
    group_slot_index = claw_cap_find_group_slot_index_locked(group_id);
    if (group_slot_index < 0) {
        claw_cap_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    if (s_runtime.group_slots[group_slot_index].state == CLAW_CAP_STATE_UNLOADING) {
        claw_cap_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_runtime.group_slots[group_slot_index].state = CLAW_CAP_STATE_DRAINING;
    {
        size_t i;
        claw_cap_group_slot_t *group_slot = &s_runtime.group_slots[group_slot_index];

        for (i = 0; i < group_slot->member_count; i++) {
            s_runtime.descriptor_slots[group_slot->member_slots[i]].state =
                CLAW_CAP_STATE_DRAINING;
        }
    }
    claw_cap_unlock();

    while (true) {
        bool active_calls;

        claw_cap_lock();
        active_calls = claw_cap_group_has_active_calls_locked((size_t)group_slot_index);
        if (!active_calls) {
            claw_cap_group_slot_t *group_slot = &s_runtime.group_slots[group_slot_index];
            size_t i;

            group_slot->state = CLAW_CAP_STATE_UNLOADING;
            for (i = 0; i < group_slot->member_count; i++) {
                s_runtime.descriptor_slots[group_slot->member_slots[i]].state =
                    CLAW_CAP_STATE_UNLOADING;
            }
            claw_cap_unlock();
            break;
        }
        claw_cap_unlock();

        if (timeout_ms != UINT32_MAX && xTaskGetTickCount() >= deadline) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(CLAW_CAP_UNLOAD_POLL_MS));
    }

    stop_err = claw_cap_stop_group_callbacks((size_t)group_slot_index);

    claw_cap_lock();
    claw_cap_clear_group_slot_locked((size_t)group_slot_index);
    claw_cap_unlock();

    return stop_err;
}

esp_err_t claw_cap_unregister(const char *id_or_name, uint32_t timeout_ms)
{
    ssize_t descriptor_slot_index;
    size_t group_slot_index;

    if (!s_runtime.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!id_or_name || !id_or_name[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    claw_cap_lock();
    descriptor_slot_index = claw_cap_find_descriptor_slot_index_locked(id_or_name);
    if (descriptor_slot_index < 0) {
        claw_cap_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    group_slot_index = s_runtime.descriptor_slots[descriptor_slot_index].group_slot_index;
    if (!s_runtime.group_slots[group_slot_index].occupied ||
            s_runtime.group_slots[group_slot_index].member_count != 1) {
        claw_cap_unlock();
        return ESP_ERR_NOT_SUPPORTED;
    }
    claw_cap_unlock();

    return claw_cap_unregister_group(
               s_runtime.group_slots[group_slot_index].group->group_id, timeout_ms);
}

bool claw_cap_group_exists(const char *group_id)
{
    bool exists;

    if (!s_runtime.initialized) {
        return false;
    }

    claw_cap_lock();
    exists = claw_cap_find_group_slot_index_locked(group_id) >= 0;
    claw_cap_unlock();
    return exists;
}

esp_err_t claw_cap_get_group_state(const char *group_id,
                                   claw_cap_state_t *state)
{
    ssize_t group_slot_index;

    if (!s_runtime.initialized || !group_id || !state) {
        return ESP_ERR_INVALID_ARG;
    }

    claw_cap_lock();
    group_slot_index = claw_cap_find_group_slot_index_locked(group_id);
    if (group_slot_index < 0) {
        claw_cap_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    *state = s_runtime.group_slots[group_slot_index].state;
    claw_cap_unlock();
    return ESP_OK;
}

esp_err_t claw_cap_get_descriptor_state(const char *id_or_name,
                                        claw_cap_descriptor_info_t *info)
{
    ssize_t descriptor_slot_index;
    claw_cap_descriptor_slot_t *slot;
    claw_cap_group_slot_t *group_slot;

    if (!s_runtime.initialized || !id_or_name || !info) {
        return ESP_ERR_INVALID_ARG;
    }

    claw_cap_lock();
    descriptor_slot_index = claw_cap_find_descriptor_slot_index_locked(id_or_name);
    if (descriptor_slot_index < 0) {
        claw_cap_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    slot = &s_runtime.descriptor_slots[descriptor_slot_index];
    group_slot = &s_runtime.group_slots[slot->group_slot_index];
    info->id = slot->descriptor.id;
    info->name = slot->descriptor.name;
    info->group_id = group_slot->group ? group_slot->group->group_id : NULL;
    info->state = slot->state;
    info->active_calls = slot->active_calls;
    claw_cap_unlock();
    return ESP_OK;
}

const claw_cap_descriptor_t *claw_cap_find(const char *id_or_name)
{
    const claw_cap_descriptor_t *result = NULL;
    ssize_t descriptor_slot_index;

    if (!s_runtime.initialized || !id_or_name || !id_or_name[0]) {
        return NULL;
    }

    claw_cap_lock();
    descriptor_slot_index = claw_cap_find_descriptor_slot_index_locked(id_or_name);
    if (descriptor_slot_index >= 0 &&
            claw_cap_descriptor_is_listable(&s_runtime.descriptor_slots[descriptor_slot_index])) {
        result = &s_runtime.descriptor_slots[descriptor_slot_index].descriptor;
    }
    claw_cap_unlock();
    return result;
}

claw_cap_list_t claw_cap_list(void)
{
    claw_cap_list_t list = {0};
    size_t i;
    size_t count = 0;

    if (!s_runtime.initialized) {
        return list;
    }

    claw_cap_lock();
    for (i = 0; i < s_runtime.descriptor_capacity; i++) {
        claw_cap_descriptor_slot_t *slot = &s_runtime.descriptor_slots[i];

        if (!claw_cap_descriptor_is_listable(slot)) {
            continue;
        }
        s_runtime.descriptor_list_snapshot[count++] = slot->descriptor;
    }
    claw_cap_unlock();

    list.items = s_runtime.descriptor_list_snapshot;
    list.count = count;
    return list;
}

claw_cap_group_list_t claw_cap_list_groups(void)
{
    claw_cap_group_list_t list = {0};
    size_t i;
    size_t count = 0;

    if (!s_runtime.initialized) {
        return list;
    }

    claw_cap_lock();
    for (i = 0; i < s_runtime.group_capacity; i++) {
        claw_cap_group_slot_t *slot = &s_runtime.group_slots[i];

        if (!slot->occupied || !slot->group) {
            continue;
        }

        s_runtime.group_list_snapshot[count].group_id = slot->group->group_id;
        s_runtime.group_list_snapshot[count].plugin_name = slot->group->plugin_name;
        s_runtime.group_list_snapshot[count].version = slot->group->version;
        s_runtime.group_list_snapshot[count].state = slot->state;
        s_runtime.group_list_snapshot[count].descriptor_count = slot->member_count;
        count++;
    }
    claw_cap_unlock();

    list.items = s_runtime.group_list_snapshot;
    list.count = count;
    return list;
}

esp_err_t claw_cap_call(const char *id_or_name,
                        const char *input_json,
                        const claw_cap_call_context_t *ctx,
                        char *output,
                        size_t output_size)
{
    claw_cap_execute_fn execute = NULL;
    const char *name = NULL;
    ssize_t descriptor_slot_index;
    esp_err_t err;

    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    output[0] = '\0';

    claw_cap_lock();
    descriptor_slot_index = claw_cap_find_descriptor_slot_index_locked(id_or_name);
    if (descriptor_slot_index < 0) {
        claw_cap_unlock();
        snprintf(output, output_size, "Error: unknown cap '%s'",
                 id_or_name ? id_or_name : "");
        return ESP_ERR_NOT_FOUND;
    }

    {
        claw_cap_descriptor_slot_t *slot = &s_runtime.descriptor_slots[descriptor_slot_index];

        if (!claw_cap_descriptor_is_available(slot) || !slot->descriptor.execute) {
            claw_cap_unlock();
            snprintf(output, output_size, "Error: cap '%s' is not available",
                     id_or_name ? id_or_name : "");
            return ESP_ERR_INVALID_STATE;
        }

        slot->active_calls++;
        execute = slot->descriptor.execute;
        name = slot->descriptor.name;
    }
    claw_cap_unlock();

    err = execute(input_json ? input_json : "{}", ctx, output, output_size);

    claw_cap_lock();
    if (descriptor_slot_index >= 0 &&
            descriptor_slot_index < (ssize_t)s_runtime.descriptor_capacity &&
            s_runtime.descriptor_slots[descriptor_slot_index].occupied &&
            s_runtime.descriptor_slots[descriptor_slot_index].active_calls > 0) {
        s_runtime.descriptor_slots[descriptor_slot_index].active_calls--;
    }
    claw_cap_unlock();

    if (err != ESP_OK && !output[0]) {
        snprintf(output, output_size, "Error: %s failed: %s",
                 name ? name : "cap",
                 esp_err_to_name(err));
    }

    return err;
}

char *claw_cap_build_catalog(void)
{
    char *buf = NULL;
    size_t cap = 512;
    size_t off = 0;
    size_t i;

    if (!s_runtime.initialized) {
        return NULL;
    }

    buf = calloc(1, cap);
    if (!buf) {
        return NULL;
    }

    off += snprintf(buf + off, cap - off, "Registered capabilities:\n");

    claw_cap_lock();
    for (i = 0; i < s_runtime.descriptor_capacity; i++) {
        claw_cap_descriptor_slot_t *slot = &s_runtime.descriptor_slots[i];
        int written;

        if (!claw_cap_descriptor_is_listable(slot)) {
            continue;
        }

        written = snprintf(buf + off, cap - off, "- %s [%s]: %s\n",
                           slot->descriptor.name,
                           slot->descriptor.family ? slot->descriptor.family : "cap",
                           slot->descriptor.description ? slot->descriptor.description : "");
        if (written < 0) {
            claw_cap_unlock();
            free(buf);
            return NULL;
        }
        if ((size_t)written >= cap - off) {
            char *tmp = realloc(buf, cap * 2);

            if (!tmp) {
                claw_cap_unlock();
                free(buf);
                return NULL;
            }
            memset(tmp + cap, 0, cap);
            buf = tmp;
            cap *= 2;
            i--;
            continue;
        }
        off += (size_t)written;
    }
    claw_cap_unlock();

    return buf;
}
