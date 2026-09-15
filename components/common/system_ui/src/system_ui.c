/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "system_ui_private.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "display_service.h"
#include "esp_check.h"
#include "esp_log.h"

EXT_RAM_BSS_ATTR system_ui_state_t s_ui;

static const char *SYSTEM_UI_DISPLAY_OWNER_NAME = "system_ui";
static int32_t s_touch_start_x;
static int32_t s_touch_start_y;

#define SYSTEM_UI_JOBS_SWIPE_EDGE_PX 32
#define SYSTEM_UI_JOBS_SWIPE_TRIGGER_PX 54
#define SYSTEM_UI_JOBS_SWIPE_HORIZONTAL_TOL_PX 48
#define SYSTEM_UI_BOTTOM_SWIPE_EDGE_PX 40
#define SYSTEM_UI_BOTTOM_SWIPE_TRIGGER_PX 54
#define SYSTEM_UI_BOTTOM_SWIPE_HORIZONTAL_TOL_PX 64
#define SYSTEM_UI_EVENT_TASK_STACK 6144
#define SYSTEM_UI_EVENT_TASK_PRIO 4

static int32_t system_ui_abs_i32(int32_t value)
{
    return value < 0 ? -value : value;
}

esp_err_t system_ui_callback_lock(void)
{
    if (s_ui.callback_mutex == NULL) {
        s_ui.callback_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_ui.callback_mutex != NULL, ESP_ERR_NO_MEM,
                            SYSTEM_UI_TAG, "create callback mutex failed");
    }
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_ui.callback_mutex, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT, SYSTEM_UI_TAG, "callback mutex timeout");
    return ESP_OK;
}

void system_ui_callback_unlock(void)
{
    if (s_ui.callback_mutex != NULL) {
        xSemaphoreGive(s_ui.callback_mutex);
    }
}

esp_err_t system_ui_post_work_event(const system_ui_work_event_t *event, TickType_t wait_ticks)
{
    ESP_RETURN_ON_FALSE(event != NULL, ESP_ERR_INVALID_ARG, SYSTEM_UI_TAG, "work event missing");
    ESP_RETURN_ON_FALSE(s_ui.event_queue != NULL, ESP_ERR_INVALID_STATE,
                        SYSTEM_UI_TAG, "event queue not ready");
    ESP_RETURN_ON_FALSE(xQueueSend(s_ui.event_queue, event, wait_ticks) == pdTRUE,
                        ESP_ERR_TIMEOUT, SYSTEM_UI_TAG, "event queue full");
    return ESP_OK;
}

static void system_ui_handle_jobs_refresh_event(uint32_t generation)
{
    system_ui_job_item_t items[SYSTEM_UI_JOBS_MAX_ITEMS] = {0};
    size_t count = 0;
    system_ui_jobs_provider_cb_t provider_cb = NULL;
    void *provider_user_ctx = NULL;

    if (system_ui_callback_lock() == ESP_OK) {
        provider_cb = s_ui.jobs_provider_cb;
        provider_user_ctx = s_ui.jobs_provider_user_ctx;
        system_ui_callback_unlock();
    }

    if (provider_cb != NULL) {
        count = provider_cb(items, SYSTEM_UI_JOBS_MAX_ITEMS, provider_user_ctx);
        if (count > SYSTEM_UI_JOBS_MAX_ITEMS) {
            count = SYSTEM_UI_JOBS_MAX_ITEMS;
        }
    }

    if (system_ui_lock() == ESP_OK) {
        if (s_ui.started && generation == s_ui.generation) {
            s_ui.jobs_refresh_running = false;
            system_ui_jobs_refresh_snapshot_locked(items, count);
            if (s_ui.jobs_refresh_pending) {
                system_ui_jobs_request_refresh_locked();
            }
        }
        system_ui_unlock();
    }
}

static void system_ui_handle_jobs_action_event(const system_ui_work_event_t *event)
{
    if (event->generation != s_ui.generation) {
        return;
    }
    if (event->jobs_action.stop_all) {
        if (event->jobs_action.stop_all_cb != NULL) {
            (void)event->jobs_action.stop_all_cb(event->jobs_action.stop_all_user_ctx);
        }
    } else if (event->jobs_action.action_cb != NULL && event->jobs_action.job_id[0]) {
        (void)event->jobs_action.action_cb(event->jobs_action.job_id,
                                           event->jobs_action.action_user_ctx);
    }
    system_ui_handle_jobs_refresh_event(event->generation);
    if (system_ui_lock() == ESP_OK) {
        if (s_ui.started && event->generation == s_ui.generation) {
            s_ui.jobs_stop_task_running = false;
        }
        system_ui_unlock();
    }
}

static void system_ui_handle_network_status_event(const system_ui_work_event_t *event)
{
    if (system_ui_lock() != ESP_OK) {
        return;
    }
    if (s_ui.started && event->generation == s_ui.generation) {
        s_ui.sta_connected = event->network_status.sta_connected;
        strlcpy(s_ui.ap_ssid, event->network_status.ap_ssid, sizeof(s_ui.ap_ssid));
        system_ui_home_update_locked();
    }
    system_ui_unlock();
}

static void system_ui_handle_screen_text_event(const system_ui_work_event_t *event)
{
    char text[193];

    /* Emoji are ordinary glyphs now: the notice font chains the monochrome
       emoji subset as its fallback, so the text is rendered verbatim and the
       firmware no longer has to strip characters or fake them with shapes. */
    strlcpy(text, event->screen_text.text, sizeof(text));

    if (system_ui_lock() != ESP_OK) {
        return;
    }
    if (s_ui.started && event->generation == s_ui.generation && s_ui.home_tile && s_ui.font) {
        if (s_ui.notice_label) {
            lv_obj_del(s_ui.notice_label);
        }
        s_ui.notice_label = lv_label_create(s_ui.home_tile);
        if (s_ui.notice_label) {
            lv_label_set_text(s_ui.notice_label, text);
            lv_label_set_long_mode(s_ui.notice_label, LV_LABEL_LONG_WRAP);
            lv_obj_set_size(s_ui.notice_label, s_ui.width - 8, 48);
            lv_obj_set_style_text_font(s_ui.notice_label,
                                       s_ui.notice_font ? s_ui.notice_font : s_ui.font, 0);
            lv_obj_set_style_text_color(s_ui.notice_label, lv_color_white(), 0);
            lv_obj_set_style_text_align(s_ui.notice_label, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(s_ui.notice_label, LV_ALIGN_BOTTOM_MID, 0, -4);
        }
    }
    system_ui_unlock();
}

static void system_ui_event_task(void *arg)
{
    (void)arg;
    system_ui_work_event_t event = {0};

    while (xQueueReceive(s_ui.event_queue, &event, portMAX_DELAY) == pdTRUE) {
        if (event.type == SYSTEM_UI_WORK_EVENT_STOP || s_ui.event_task_stop) {
            break;
        }

        switch (event.type) {
        case SYSTEM_UI_WORK_EVENT_SHOW_JOBS:
            if (system_ui_lock() == ESP_OK) {
                if (s_ui.started && event.generation == s_ui.generation) {
                    system_ui_jobs_set_visible_locked(true);
                }
                system_ui_unlock();
            }
            break;
        case SYSTEM_UI_WORK_EVENT_LAUNCHER_SELECT:
        {
            system_ui_launcher_select_cb_t cb = NULL;
            void *user_ctx = NULL;
            if (event.generation == s_ui.generation && system_ui_callback_lock() == ESP_OK) {
                cb = s_ui.launcher_select_cb;
                user_ctx = s_ui.launcher_select_user_ctx;
                system_ui_callback_unlock();
            }
            if (cb != NULL) {
                event.launcher_selection.id = event.launcher_id;
                event.launcher_selection.title = event.launcher_title;
                event.launcher_selection.action = event.launcher_action;
                event.launcher_selection.args_json =
                    event.launcher_args_json[0] ? event.launcher_args_json : NULL;
                cb(&event.launcher_selection, user_ctx);
            }
            break;
        }
        case SYSTEM_UI_WORK_EVENT_JOBS_REFRESH:
            system_ui_handle_jobs_refresh_event(event.generation);
            break;
        case SYSTEM_UI_WORK_EVENT_JOBS_ACTION:
            system_ui_handle_jobs_action_event(&event);
            break;
        case SYSTEM_UI_WORK_EVENT_NETWORK_STATUS:
            system_ui_handle_network_status_event(&event);
            break;
        case SYSTEM_UI_WORK_EVENT_SCREEN_TEXT:
            system_ui_handle_screen_text_event(&event);
            break;
        case SYSTEM_UI_WORK_EVENT_FULLSCREEN_ENTER:
            (void)system_ui_fullscreen_enter_locked();
            break;
        case SYSTEM_UI_WORK_EVENT_FULLSCREEN_TEXT:
            (void)system_ui_fullscreen_text_locked(event.screen_text.text, event.screen_text.orientation);
            break;
        case SYSTEM_UI_WORK_EVENT_FULLSCREEN_CLEAR:
            (void)system_ui_fullscreen_clear_locked();
            break;
        case SYSTEM_UI_WORK_EVENT_FULLSCREEN_EXIT:
            (void)system_ui_fullscreen_exit_locked();
            break;
        case SYSTEM_UI_WORK_EVENT_APP_EXIT_SWIPE:
        {
            system_ui_app_exit_swipe_cb_t cb = NULL;
            void *user_ctx = NULL;
            if (event.generation == s_ui.generation && system_ui_callback_lock() == ESP_OK) {
                cb = s_ui.app_exit_swipe_cb;
                user_ctx = s_ui.app_exit_swipe_user_ctx;
                system_ui_callback_unlock();
            }
            if (cb != NULL) {
                esp_err_t err = cb(user_ctx);
                if (err != ESP_OK) {
                    ESP_LOGW(SYSTEM_UI_TAG, "app exit swipe callback failed: %s", esp_err_to_name(err));
                }
            }
            break;
        }
        case SYSTEM_UI_WORK_EVENT_STOP:
        default:
            break;
        }
    }

    s_ui.event_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t system_ui_start_event_task(void)
{
    if (s_ui.event_queue == NULL) {
        s_ui.event_queue = xQueueCreate(SYSTEM_UI_EVENT_QUEUE_LEN,
                                        sizeof(system_ui_work_event_t));
        ESP_RETURN_ON_FALSE(s_ui.event_queue != NULL, ESP_ERR_NO_MEM,
                            SYSTEM_UI_TAG, "create event queue failed");
    }
    if (s_ui.event_task != NULL) {
        return ESP_OK;
    }

    s_ui.event_task_stop = false;
    BaseType_t ok = xTaskCreate(system_ui_event_task,
                                "ui_event",
                                SYSTEM_UI_EVENT_TASK_STACK,
                                NULL,
                                SYSTEM_UI_EVENT_TASK_PRIO,
                                &s_ui.event_task);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM,
                        SYSTEM_UI_TAG, "create event task failed");
    return ESP_OK;
}

static void system_ui_stop_event_task(void)
{
    TaskHandle_t task = s_ui.event_task;

    if (task != NULL && s_ui.event_queue != NULL) {
        system_ui_work_event_t event = {
            .type = SYSTEM_UI_WORK_EVENT_STOP,
            .generation = s_ui.generation,
        };
        s_ui.event_task_stop = true;
        (void)xQueueSend(s_ui.event_queue, &event, 0);

        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(SYSTEM_UI_STOP_TIMEOUT_MS);
        while (s_ui.event_task != NULL && xTaskGetTickCount() < deadline) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        if (s_ui.event_task != NULL) {
            vTaskDelete(s_ui.event_task);
            s_ui.event_task = NULL;
        }
    }

    if (s_ui.event_queue != NULL) {
        vQueueDelete(s_ui.event_queue);
        s_ui.event_queue = NULL;
    }
    s_ui.event_task_stop = false;
}

static void system_ui_touch_observer_cb(const display_service_touch_sample_t *sample, void *user_ctx)
{
    (void)user_ctx;

    if (sample == NULL || !s_ui.started) {
        return;
    }
    if (sample->pressed) {
        if (s_ui.touch_gesture == SYSTEM_UI_TOUCH_GESTURE_NONE) {
            bool exclusive = display_service_has_exclusive_session();
            s_touch_start_x = sample->x;
            s_touch_start_y = sample->y;
            if (exclusive && s_touch_start_y >= (int32_t)s_ui.height - SYSTEM_UI_BOTTOM_SWIPE_EDGE_PX) {
                s_ui.touch_gesture = SYSTEM_UI_TOUCH_GESTURE_EXIT_APP;
            } else if (!exclusive && system_ui_system_overlay_allowed() && s_touch_start_y <= SYSTEM_UI_JOBS_SWIPE_EDGE_PX) {
                s_ui.touch_gesture = SYSTEM_UI_TOUCH_GESTURE_SHOW_JOBS;
            }
        } else if (s_ui.touch_gesture == SYSTEM_UI_TOUCH_GESTURE_SHOW_JOBS &&
                   !s_ui.jobs_visible &&
                   sample->y - s_touch_start_y >= SYSTEM_UI_JOBS_SWIPE_TRIGGER_PX &&
                   system_ui_abs_i32(sample->x - s_touch_start_x) <= SYSTEM_UI_JOBS_SWIPE_HORIZONTAL_TOL_PX) {
            system_ui_work_event_t event = {
                .type = SYSTEM_UI_WORK_EVENT_SHOW_JOBS,
                .generation = s_ui.generation,
            };
            (void)system_ui_post_work_event(&event, 0);
            s_ui.touch_gesture = SYSTEM_UI_TOUCH_GESTURE_NONE;
        } else if (s_ui.touch_gesture == SYSTEM_UI_TOUCH_GESTURE_EXIT_APP &&
                   s_touch_start_y - sample->y >= SYSTEM_UI_BOTTOM_SWIPE_TRIGGER_PX &&
                   system_ui_abs_i32(sample->x - s_touch_start_x) <= SYSTEM_UI_BOTTOM_SWIPE_HORIZONTAL_TOL_PX) {
            system_ui_work_event_t event = {
                .type = SYSTEM_UI_WORK_EVENT_APP_EXIT_SWIPE,
                .generation = s_ui.generation,
            };
            (void)system_ui_post_work_event(&event, 0);
            s_ui.touch_gesture = SYSTEM_UI_TOUCH_GESTURE_NONE;
        }
    } else {
        s_ui.touch_gesture = SYSTEM_UI_TOUCH_GESTURE_NONE;
    }
}

bool system_ui_system_overlay_allowed(void)
{
    return display_service_exclusive_allows_system_overlay();
}

static void system_ui_state_observer_cb(display_service_state_event_t event, void *user_ctx)
{
    (void)user_ctx;

    if (!s_ui.started) {
        return;
    }

    if (system_ui_lock() != ESP_OK) {
        return;
    }

    switch (event) {
    case DISPLAY_SERVICE_STATE_EVENT_EXCLUSIVE_LVGL_ENTERED:
        if (!display_service_exclusive_allows_system_overlay()) {
            if (s_ui.jobs_visible) {
                system_ui_jobs_set_visible_locked(false);
            }
        }
        break;
    case DISPLAY_SERVICE_STATE_EVENT_EXCLUSIVE_RAW_ENTERED:
        break;
    case DISPLAY_SERVICE_STATE_EVENT_EXCLUSIVE_LVGL_EXITED:
    case DISPLAY_SERVICE_STATE_EVENT_EXCLUSIVE_RAW_EXITED:
        break;
    default:
        break;
    }

    system_ui_unlock();
}

static bool system_ui_system_overlay_visible_locked(void)
{
    return s_ui.jobs_visible;
}

void system_ui_dummy_draw_suspend_for_overlay_locked(void)
{
    display_service_exclusive_raw_suspend_locked();
}

void system_ui_dummy_draw_resume_if_no_overlay_locked(void)
{
    if (!system_ui_system_overlay_visible_locked()) {
        display_service_exclusive_raw_resume_locked();
    }
}

static void system_ui_clear_callbacks_locked(void)
{
    if (system_ui_callback_lock() != ESP_OK) {
        ESP_LOGW(SYSTEM_UI_TAG, "clear callbacks skipped: callback lock failed");
        return;
    }
    s_ui.jobs_provider_cb = NULL;
    s_ui.jobs_provider_user_ctx = NULL;
    s_ui.jobs_action_cb = NULL;
    s_ui.jobs_action_user_ctx = NULL;
    s_ui.jobs_stop_all_cb = NULL;
    s_ui.jobs_stop_all_user_ctx = NULL;
    s_ui.app_exit_swipe_cb = NULL;
    s_ui.app_exit_swipe_user_ctx = NULL;
    s_ui.launcher_select_cb = NULL;
    s_ui.launcher_select_user_ctx = NULL;
    system_ui_callback_unlock();
}

static void system_ui_delete_ui_locked(void)
{
    s_ui.started = false;
    display_service_set_default_screen_locked(NULL);
    system_ui_delete_jobs_locked();
    system_ui_delete_overlay_locked();
    system_ui_delete_home_locked();
    system_ui_destroy_font_locked();
    system_ui_clear_callbacks_locked();
}

esp_err_t system_ui_start(const system_ui_config_t *config)
{
    esp_err_t ret;
    display_service_session_config_t display_session_config = {
        .owner_name = SYSTEM_UI_DISPLAY_OWNER_NAME,
        .mode = DISPLAY_SERVICE_MODE_SHARED_LVGL,
        .display_config = {
            .buffer_lines = config ? config->buffer_lines : 0,
            .tick_ms = config ? config->tick_ms : 0,
            .task_period_ms = config ? config->task_period_ms : 0,
        },
    };

    if (s_ui.started) {
        return ESP_OK;
    }

    s_ui.generation++;
    ESP_RETURN_ON_ERROR(system_ui_callback_lock(), SYSTEM_UI_TAG, "init callback mutex failed");
    system_ui_callback_unlock();
    ESP_RETURN_ON_ERROR(display_service_open(&display_session_config, &s_ui.display_session),
                        SYSTEM_UI_TAG, "open display session failed");
    lv_display_t *display = display_service_session_display(s_ui.display_session);
    ESP_GOTO_ON_FALSE(display != NULL, ESP_ERR_INVALID_STATE, fail, SYSTEM_UI_TAG, "display service LVGL display is NULL");
    s_ui.width = (uint32_t)lv_display_get_horizontal_resolution(display);
    s_ui.height = (uint32_t)lv_display_get_vertical_resolution(display);
    ESP_GOTO_ON_FALSE(s_ui.width > 0 && s_ui.height > 0, ESP_ERR_INVALID_STATE, fail, SYSTEM_UI_TAG, "display resolution is invalid");
    ESP_GOTO_ON_ERROR(system_ui_start_event_task(), fail, SYSTEM_UI_TAG, "start event task failed");
    ESP_GOTO_ON_ERROR(display_service_set_touch_observer(system_ui_touch_observer_cb, NULL),
                      fail, SYSTEM_UI_TAG, "set touch observer failed");
    ESP_GOTO_ON_ERROR(display_service_set_state_observer(system_ui_state_observer_cb, NULL),
                      fail, SYSTEM_UI_TAG, "set state observer failed");
    ESP_GOTO_ON_ERROR(system_ui_vibration_init(), fail, SYSTEM_UI_TAG, "init vibration failed");

    ESP_GOTO_ON_ERROR(system_ui_lock(), fail, SYSTEM_UI_TAG, "lock failed");
    system_ui_create_font_locked(config && config->font_path ? config->font_path : SYSTEM_UI_DEFAULT_FONT_PATH,
                                  config ? config->font_size : SYSTEM_UI_DEFAULT_FONT_SIZE);
    ret = system_ui_create_home_locked();
    if (ret == ESP_OK) {
        display_service_set_default_screen_locked(s_ui.home_screen);
    }
    if (ret == ESP_OK) {
        ret = system_ui_create_overlay_locked();
    }
    if (ret == ESP_OK) {
        ret = system_ui_create_jobs_locked();
    }
    s_ui.started = ret == ESP_OK;
    system_ui_unlock();
    ESP_GOTO_ON_ERROR(ret, fail, SYSTEM_UI_TAG, "create UI failed");

    return ESP_OK;

fail:
    system_ui_stop();
    return ret;
}

void system_ui_stop(void)
{
    s_ui.generation++;
    system_ui_stop_event_task();

    if (display_service_is_started() && system_ui_lock() == ESP_OK) {
        system_ui_delete_ui_locked();
        system_ui_unlock();
    } else {
        s_ui.started = false;
    }
    (void)display_service_set_touch_observer(NULL, NULL);
    (void)display_service_set_state_observer(NULL, NULL);
    system_ui_vibration_deinit();
    s_ui.touch_gesture = SYSTEM_UI_TOUCH_GESTURE_NONE;
    if (s_ui.display_session != NULL) {
        (void)display_service_close(s_ui.display_session);
        s_ui.display_session = NULL;
    }
}

bool system_ui_is_started(void)
{
    bool started = false;

    if (!display_service_is_started()) {
        return false;
    }
    if (system_ui_lock() == ESP_OK) {
        started = s_ui.started;
        system_ui_unlock();
    }
    return started;
}

esp_err_t system_ui_set_callbacks(const system_ui_callbacks_t *callbacks, void *user_ctx)
{
    ESP_RETURN_ON_FALSE(callbacks != NULL, ESP_ERR_INVALID_ARG, SYSTEM_UI_TAG, "system UI callbacks missing");
    ESP_RETURN_ON_ERROR(system_ui_launcher_set_select_callback(callbacks->on_launcher_select, user_ctx),
                        SYSTEM_UI_TAG, "set launcher callback failed");
    ESP_RETURN_ON_ERROR(system_ui_jobs_set_provider(callbacks->get_tasks, user_ctx),
                        SYSTEM_UI_TAG, "set task provider failed");
    ESP_RETURN_ON_ERROR(system_ui_jobs_set_action_callback(callbacks->on_stop_task, user_ctx),
                        SYSTEM_UI_TAG, "set task action callback failed");
    ESP_RETURN_ON_ERROR(system_ui_jobs_set_stop_all_callback(callbacks->on_stop_all_tasks, user_ctx),
                        SYSTEM_UI_TAG, "set stop-all task callback failed");
    ESP_RETURN_ON_ERROR(system_ui_callback_lock(), SYSTEM_UI_TAG, "callback lock failed");
    s_ui.app_exit_swipe_cb = callbacks->on_app_exit_swipe;
    s_ui.app_exit_swipe_user_ctx = user_ctx;
    system_ui_callback_unlock();
    return ESP_OK;
}

esp_err_t system_ui_update_network(const system_ui_network_state_t *state)
{
    ESP_RETURN_ON_FALSE(state != NULL, ESP_ERR_INVALID_ARG, SYSTEM_UI_TAG, "network state missing");
    return system_ui_set_network_status(state->sta_connected, state->ap_ssid);
}

esp_err_t system_ui_set_activity(bool active)
{
    return system_ui_overlay_set_visible(active);
}

esp_err_t system_ui_show_task_panel(bool visible)
{
    return system_ui_jobs_set_visible(visible);
}

esp_err_t system_ui_refresh_tasks(void)
{
    return system_ui_jobs_request_refresh();
}

esp_err_t system_ui_lock(void)
{
    return display_service_lock();
}

void system_ui_unlock(void)
{
    display_service_unlock();
}

esp_err_t system_ui_fullscreen_enter_locked(void)
{
    ESP_RETURN_ON_FALSE(s_ui.started && s_ui.home_screen, ESP_ERR_INVALID_STATE, SYSTEM_UI_TAG, "ui not started");
    if (s_ui.fullscreen_screen) return ESP_OK;
    s_ui.fullscreen_screen = lv_obj_create(NULL);
    ESP_RETURN_ON_FALSE(s_ui.fullscreen_screen != NULL, ESP_ERR_NO_MEM, SYSTEM_UI_TAG, "create fullscreen failed");
    lv_obj_set_style_bg_color(s_ui.fullscreen_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_ui.fullscreen_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ui.fullscreen_screen, 0, 0);
    lv_obj_set_style_pad_all(s_ui.fullscreen_screen, 0, 0);
    display_service_set_default_screen_locked(s_ui.fullscreen_screen);
    system_ui_load_screen_locked(s_ui.fullscreen_screen);
    return ESP_OK;
}

/* Landscape is realised as a 90-degree transform on the label itself rather
 * than via lv_display_set_rotation(). The panel is a fixed 135x240 portrait
 * ST7789 and the SPI adapter latches its rotation at registration time, so
 * rotating the logical display here would desynchronise the flush geometry and
 * corrupt frames once the fullscreen page is torn down. Rotating the object
 * leaves the whole display flush path untouched.
 *
 * Two LVGL details this depends on: transform_rotation is expressed in 0.1
 * degree units (900 == 90 deg), and the transform pivot defaults to the
 * widget's *top-left corner* - leaving it there would swing the label off
 * screen, so the pivot has to be moved to the centre explicitly. */
#define SYSTEM_UI_LANDSCAPE_ROTATION_DEG10 900

static bool system_ui_orientation_is_landscape(const char *orientation)
{
    return orientation != NULL && strcasecmp(orientation, "landscape") == 0;
}

static bool system_ui_orientation_is_known(const char *orientation)
{
    if (orientation == NULL || orientation[0] == '\0') return true;
    return strcasecmp(orientation, "portrait") == 0 || strcasecmp(orientation, "landscape") == 0;
}

esp_err_t system_ui_fullscreen_text_locked(const char *text, const char *orientation)
{
    ESP_RETURN_ON_FALSE(text != NULL && strlen(text) <= 192, ESP_ERR_INVALID_ARG, SYSTEM_UI_TAG, "fullscreen text invalid");
    if (!system_ui_orientation_is_known(orientation)) {
        /* Fail loudly rather than silently rendering portrait for a request the
           caller believes was honoured. */
        ESP_LOGW(SYSTEM_UI_TAG, "unknown orientation '%s', falling back to portrait", orientation);
    }
    const bool landscape = system_ui_orientation_is_landscape(orientation);
    ESP_RETURN_ON_ERROR(system_ui_fullscreen_enter_locked(), SYSTEM_UI_TAG, "enter fullscreen failed");
    if (s_ui.fullscreen_label) lv_obj_del(s_ui.fullscreen_label);
    s_ui.fullscreen_label = lv_label_create(s_ui.fullscreen_screen);
    ESP_RETURN_ON_FALSE(s_ui.fullscreen_label != NULL, ESP_ERR_NO_MEM, SYSTEM_UI_TAG, "create fullscreen label failed");
    /* The text is rendered verbatim: the fullscreen font carries the emoji
       subset as its fallback, so emoji draw as glyphs instead of being
       replaced by a coloured circle. */
    lv_label_set_text(s_ui.fullscreen_label, text);
    lv_label_set_long_mode(s_ui.fullscreen_label, LV_LABEL_LONG_WRAP);

    /* In landscape the label is laid out with its axes swapped and then rotated
       as a whole, so text wraps along the panel's long edge. */
    const int32_t label_w = landscape ? (int32_t)s_ui.height - 8 : (int32_t)s_ui.width - 8;
    const int32_t label_h = landscape ? (int32_t)s_ui.width - 8 : (int32_t)s_ui.height - 8;
    lv_obj_set_size(s_ui.fullscreen_label, label_w, label_h);

    lv_obj_set_style_text_font(s_ui.fullscreen_label, s_ui.notice_font ? s_ui.notice_font : s_ui.font, 0);
    lv_obj_set_style_text_color(s_ui.fullscreen_label, lv_color_white(), 0);
    lv_obj_set_style_text_align(s_ui.fullscreen_label, LV_TEXT_ALIGN_CENTER, 0);

    if (landscape) {
        /* Pivot derived from the size we just requested rather than from
           lv_obj_get_width(): the object's coords are not recomputed until the
           next layout pass, so the getter would still report the old size. */
        lv_obj_set_style_transform_pivot_x(s_ui.fullscreen_label, label_w / 2, 0);
        lv_obj_set_style_transform_pivot_y(s_ui.fullscreen_label, label_h / 2, 0);
        lv_obj_set_style_transform_rotation(s_ui.fullscreen_label, SYSTEM_UI_LANDSCAPE_ROTATION_DEG10, 0);
    }
    lv_obj_align(s_ui.fullscreen_label, LV_ALIGN_CENTER, 0, 0);
    return ESP_OK;
}

esp_err_t system_ui_fullscreen_clear_locked(void)
{
    ESP_RETURN_ON_FALSE(s_ui.fullscreen_screen != NULL, ESP_ERR_INVALID_STATE, SYSTEM_UI_TAG, "fullscreen not active");
    if (s_ui.fullscreen_label) { lv_obj_del(s_ui.fullscreen_label); s_ui.fullscreen_label = NULL; }
    return ESP_OK;
}

esp_err_t system_ui_fullscreen_exit_locked(void)
{
    if (s_ui.fullscreen_screen) { lv_obj_del(s_ui.fullscreen_screen); s_ui.fullscreen_screen = NULL; }
    s_ui.fullscreen_label = NULL;
    display_service_set_default_screen_locked(s_ui.home_screen);
    system_ui_load_screen_locked(s_ui.home_screen);
    return ESP_OK;
}

esp_err_t system_ui_fullscreen_enter(void)
{
    system_ui_work_event_t event = {.type = SYSTEM_UI_WORK_EVENT_FULLSCREEN_ENTER, .generation = s_ui.generation};
    return system_ui_post_work_event(&event, pdMS_TO_TICKS(100));
}

esp_err_t system_ui_fullscreen_text(const char *text, const char *orientation)
{
    system_ui_work_event_t event = {.type = SYSTEM_UI_WORK_EVENT_FULLSCREEN_TEXT, .generation = s_ui.generation};
    ESP_RETURN_ON_FALSE(text != NULL && strlen(text) <= 192, ESP_ERR_INVALID_ARG, SYSTEM_UI_TAG, "fullscreen text invalid");
    strlcpy(event.screen_text.text, text, sizeof(event.screen_text.text));
    strlcpy(event.screen_text.orientation, orientation ? orientation : "portrait", sizeof(event.screen_text.orientation));
    return system_ui_post_work_event(&event, pdMS_TO_TICKS(100));
}

esp_err_t system_ui_fullscreen_clear(void)
{
    system_ui_work_event_t event = {.type = SYSTEM_UI_WORK_EVENT_FULLSCREEN_CLEAR, .generation = s_ui.generation};
    return system_ui_post_work_event(&event, pdMS_TO_TICKS(100));
}

esp_err_t system_ui_fullscreen_exit(void)
{
    system_ui_work_event_t event = {.type = SYSTEM_UI_WORK_EVENT_FULLSCREEN_EXIT, .generation = s_ui.generation};
    return system_ui_post_work_event(&event, pdMS_TO_TICKS(100));
}

esp_err_t system_ui_show_text(const char *text)
{
    ESP_RETURN_ON_FALSE(text != NULL && text[0] != '\0', ESP_ERR_INVALID_ARG,
                        SYSTEM_UI_TAG, "screen text missing");
    ESP_RETURN_ON_FALSE(strlen(text) <= 192, ESP_ERR_INVALID_SIZE,
                        SYSTEM_UI_TAG, "screen text too long");

    system_ui_work_event_t event = {.type = SYSTEM_UI_WORK_EVENT_SCREEN_TEXT,
                                    .generation = s_ui.generation};
    strlcpy(event.screen_text.text, text, sizeof(event.screen_text.text));
    return system_ui_post_work_event(&event, pdMS_TO_TICKS(100));
}

esp_err_t system_ui_text_coverage(const char *text, size_t *out_missing, uint32_t *out_first)
{
    ESP_RETURN_ON_FALSE(text != NULL, ESP_ERR_INVALID_ARG, SYSTEM_UI_TAG, "coverage text missing");
    if (out_missing) *out_missing = 0;
    if (out_first) *out_first = 0;

    ESP_RETURN_ON_ERROR(system_ui_lock(), SYSTEM_UI_TAG, "coverage lock failed");

    /* Resolve against the font the notice layer actually draws with, so the
       answer matches what lands on the panel. tiny_ttf's get_glyph_dsc returns
       false for a code point no font in the chain can supply, and it populates
       the glyph cache on the way - hence the lock. */
    const lv_font_t *font = s_ui.notice_font ? s_ui.notice_font : s_ui.font;
    if (font == NULL) {
        system_ui_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    size_t missing = 0;
    uint32_t first = 0;
    const char *cursor = text;
    while (*cursor != '\0') {
        uint32_t codepoint = 0;
        const size_t consumed = display_ttf_utf8_decode(cursor, &codepoint);
        if (consumed == 0) {
            cursor++;
            continue;
        }
        cursor += consumed;

        /* Control characters drive layout instead of resolving to a glyph, and
           tiny_ttf only short-circuits the ones below 0x20 - 0x7F would be
           reported as missing if it were not skipped here. */
        if (codepoint < 0x20 || codepoint == 0x7F) {
            continue;
        }

        uint32_t next = 0;
        display_ttf_utf8_decode(cursor, &next);
        lv_font_glyph_dsc_t dsc = {0};
        if (!lv_font_get_glyph_dsc(font, &dsc, codepoint, next)) {
            if (missing == 0) first = codepoint;
            missing++;
        }
    }

    system_ui_unlock();

    if (out_missing) *out_missing = missing;
    if (out_first) *out_first = first;
    return ESP_OK;
}

void system_ui_load_screen_locked(lv_obj_t *screen)
{
    if (!screen) {
        return;
    }
    if (display_service_has_exclusive_session()) {
        return;
    }
    lv_screen_load(screen);
}
