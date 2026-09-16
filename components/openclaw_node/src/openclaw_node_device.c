#include "openclaw_node_device.h"
#include "openclaw_node_identity.h"
#include "esp_chip_info.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_netif.h"
#include "esp_board_manager.h"
#include "esp_board_manager_defs.h"
#include "esp_board_periph.h"
#include "periph_ledc.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_codec_dev.h"
#include "system_ui.h"
#include "cJSON.h"
#include "cap_files.h"
#include "cap_lua.h"
#include "claw_cap.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <math.h>

static const char *s_commands[] = {
    OPENCLAW_NODE_DEVICE_COMMAND_INFO,
    OPENCLAW_NODE_DEVICE_COMMAND_STATUS,
    OPENCLAW_NODE_DEVICE_COMMAND_NETWORK,
    OPENCLAW_NODE_DEVICE_COMMAND_BACKLIGHT,
    OPENCLAW_NODE_DEVICE_COMMAND_RESTART,
    OPENCLAW_NODE_DEVICE_COMMAND_BUTTON_STATUS,
    OPENCLAW_NODE_DEVICE_COMMAND_AUDIO_VOLUME,
    OPENCLAW_NODE_DEVICE_COMMAND_AUDIO_TONE,
    OPENCLAW_NODE_DEVICE_COMMAND_SCREEN_CLEAR,
    OPENCLAW_NODE_DEVICE_COMMAND_SCREEN_TEXT,
    OPENCLAW_NODE_DEVICE_COMMAND_SCREEN_FULLSCREEN_ENTER,
    OPENCLAW_NODE_DEVICE_COMMAND_SCREEN_FULLSCREEN_TEXT,
    OPENCLAW_NODE_DEVICE_COMMAND_SCREEN_FULLSCREEN_CLEAR,
    OPENCLAW_NODE_DEVICE_COMMAND_SCREEN_FULLSCREEN_EXIT,
    OPENCLAW_NODE_COMMAND_FILES_READ,
    OPENCLAW_NODE_COMMAND_FILES_WRITE,
    OPENCLAW_NODE_COMMAND_FILES_DELETE,
    OPENCLAW_NODE_COMMAND_FILES_COPY,
    OPENCLAW_NODE_COMMAND_FILES_MOVE,
    OPENCLAW_NODE_COMMAND_FILES_LIST,
    OPENCLAW_NODE_COMMAND_LUA_RUN,
    OPENCLAW_NODE_COMMAND_LUA_RUN_ASYNC,
    OPENCLAW_NODE_COMMAND_LUA_JOBS,
    OPENCLAW_NODE_COMMAND_LUA_JOB,
    OPENCLAW_NODE_COMMAND_LUA_STOP,
    OPENCLAW_NODE_COMMAND_LUA_STOP_ALL,
};

const char *const *openclaw_node_device_commands(size_t *count)
{
    if (count) *count = sizeof(s_commands) / sizeof(s_commands[0]);
    return s_commands;
}

static esp_err_t call_existing_cap(const char *cap_name, const char *params_json,
                                   char *out, size_t out_size)
{
    claw_cap_call_context_t ctx = {
        .caller = CLAW_CAP_CALLER_SYSTEM,
        .source_cap = "openclaw_native_node",
    };
    esp_err_t err = claw_cap_call(cap_name, params_json ? params_json : "{}",
                                  &ctx, out, out_size);
    if (err != ESP_OK && out && out_size > 0 && out[0] == '\0') {
        snprintf(out, out_size, "{\"error\":\"%s\"}", esp_err_to_name(err));
    }
    return err;
}

static const char *map_native_cap(const char *command)
{
    if (strcmp(command, OPENCLAW_NODE_COMMAND_FILES_READ) == 0) return "read_file";
    if (strcmp(command, OPENCLAW_NODE_COMMAND_FILES_WRITE) == 0) return "write_file";
    if (strcmp(command, OPENCLAW_NODE_COMMAND_FILES_DELETE) == 0) return "delete_file";
    if (strcmp(command, OPENCLAW_NODE_COMMAND_FILES_COPY) == 0) return "copy_file";
    if (strcmp(command, OPENCLAW_NODE_COMMAND_FILES_MOVE) == 0) return "move_file";
    if (strcmp(command, OPENCLAW_NODE_COMMAND_FILES_LIST) == 0) return "list_dir";
    if (strcmp(command, OPENCLAW_NODE_COMMAND_LUA_RUN) == 0) return "lua_run_script";
    if (strcmp(command, OPENCLAW_NODE_COMMAND_LUA_RUN_ASYNC) == 0) return "lua_run_script_async";
    if (strcmp(command, OPENCLAW_NODE_COMMAND_LUA_JOBS) == 0) return "lua_list_async_jobs";
    if (strcmp(command, OPENCLAW_NODE_COMMAND_LUA_JOB) == 0) return "lua_get_async_job";
    if (strcmp(command, OPENCLAW_NODE_COMMAND_LUA_STOP) == 0) return "lua_stop_async_job";
    if (strcmp(command, OPENCLAW_NODE_COMMAND_LUA_STOP_ALL) == 0) return "lua_stop_all_async_jobs";
    return NULL;
}

static esp_err_t write_info(char *out, size_t size)
{
    uint8_t public_key[OPENCLAW_NODE_ED25519_PUBLIC_KEY_LEN];
    char id[OPENCLAW_NODE_ED25519_PUBLIC_KEY_LEN * 2 + 1];
    esp_chip_info_t chip = {0};
    uint8_t mac[6] = {0};
    if (openclaw_node_identity_get_public_key(public_key) != ESP_OK ||
        openclaw_node_identity_get_id(id, sizeof(id)) != ESP_OK) return ESP_ERR_INVALID_STATE;
    esp_chip_info(&chip);
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    int n = snprintf(out, size,
                     "{\"command\":\"device.info\",\"deviceId\":\"%s\","
                     "\"model\":\"M5Stack StickS3\",\"mcu\":\"ESP32-S3\","
                     "\"chipRevision\":%d,\"cores\":%d,\"flashMB\":8,\"psramMB\":8,"
                     "\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
                     "\"firmware\":\"ESP-OpenClaw\",\"firmwareVersion\":\"0.1.0\"}",
                     id, chip.revision, chip.cores, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return n < 0 || (size_t)n >= size ? ESP_ERR_INVALID_SIZE : ESP_OK;
}


static esp_err_t write_status(char *out, size_t size)
{
    int n = snprintf(out, size,
                     "{\"command\":\"device.status\",\"uptimeMs\":%llu,"
                     "\"freeHeap\":%u,\"minFreeHeap\":%u,\"freePsram\":%u,"
                     "\"psramAvailable\":%s}",
                     (unsigned long long)(esp_timer_get_time() / 1000ULL),
                     (unsigned)esp_get_free_heap_size(),
                     (unsigned)esp_get_minimum_free_heap_size(),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     esp_psram_is_initialized() ? "true" : "false");
    return n < 0 || (size_t)n >= size ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

static esp_err_t write_network(char *out, size_t size)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {0};
    char ip_text[16] = "0.0.0.0";
    char gateway_text[16] = "0.0.0.0";
    bool connected = false;
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        esp_ip4addr_ntoa(&ip.ip, ip_text, sizeof(ip_text));
        esp_ip4addr_ntoa(&ip.gw, gateway_text, sizeof(gateway_text));
        connected = ip.ip.addr != 0;
    }
    int n = snprintf(out, size,
                     "{\"command\":\"device.network\",\"connected\":%s,"
                     "\"ip\":\"%s\",\"gateway\":\"%s\"}",
                     connected ? "true" : "false", ip_text, gateway_text);
    return n < 0 || (size_t)n >= size ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

static esp_err_t set_backlight(const char *params_json, char *out, size_t size)
{
    cJSON *params = cJSON_Parse(params_json ? params_json : "{}");
    cJSON *level = params ? cJSON_GetObjectItem(params, "level") : NULL;
    void *handle_ptr = NULL;

    periph_ledc_config_t *ledc_cfg = NULL;
    periph_ledc_handle_t *ledc_handle = NULL;
    esp_err_t err = ESP_OK;
    if (!cJSON_IsNumber(level) || level->valuedouble < 0 || level->valuedouble > 100 ||
        level->valuedouble != (int)level->valuedouble) {
        cJSON_Delete(params);
        return ESP_ERR_INVALID_ARG;
    }
    /* Copy out of the parsed tree before it is freed below: `level` points into
       `params`, so reading it after cJSON_Delete() is a use-after-free. */
    const int level_value = (int)level->valuedouble;
    err = esp_board_manager_get_periph_handle("ledc_backlight", &handle_ptr);
    if (err == ESP_OK) {
        ledc_handle = (periph_ledc_handle_t *)handle_ptr;
        err = esp_board_periph_get_config("ledc_backlight", (void **)&ledc_cfg);
    }
    if (err == ESP_OK) {
        uint32_t max_duty = (1U << (uint32_t)ledc_cfg->duty_resolution) - 1U;
        uint32_t duty = ((uint32_t)level_value * max_duty) / 100U;
        err = ledc_set_duty(ledc_handle->speed_mode, ledc_handle->channel, duty);
        if (err == ESP_OK) err = ledc_update_duty(ledc_handle->speed_mode, ledc_handle->channel);
    }
    cJSON_Delete(params);
    if (err != ESP_OK) return err;
    int n = snprintf(out, size, "{\"command\":\"device.backlight\",\"level\":%d}", level_value);
    return n < 0 || (size_t)n >= size ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

static esp_err_t button_status(char *out, size_t size)
{
    int key1 = gpio_get_level(GPIO_NUM_11);
    int key2 = gpio_get_level(GPIO_NUM_12);
    int n = snprintf(out, size,
                     "{\"command\":\"device.button.status\",\"button1\":{\"gpio\":11,\"pressed\":%s},"
                     "\"button2\":{\"gpio\":12,\"pressed\":%s}}",
                     key1 == 0 ? "true" : "false", key2 == 0 ? "true" : "false");
    return n < 0 || (size_t)n >= size ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

static esp_err_t audio_volume(const char *params_json, char *out, size_t size)
{
    cJSON *params = cJSON_Parse(params_json ? params_json : "{}");
    cJSON *volume = params ? cJSON_GetObjectItem(params, "volume") : NULL;
    void *device = NULL;
    int value;
    esp_err_t err = ESP_OK;
    if (!cJSON_IsNumber(volume) || volume->valuedouble < 0 || volume->valuedouble > 100 ||
        volume->valuedouble != (int)volume->valuedouble) {
        cJSON_Delete(params);
        return ESP_ERR_INVALID_ARG;
    }
    value = (int)volume->valuedouble;
    err = esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_AUDIO_DAC, &device);
    if (err == ESP_OK && device) {
        esp_codec_dev_handle_t codec = *(esp_codec_dev_handle_t *)device;
        err = esp_codec_dev_set_out_vol(codec, value) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
    }
    cJSON_Delete(params);
    if (err != ESP_OK) return err;
    int n = snprintf(out, size, "{\"command\":\"audio.volume\",\"volume\":%d}", value);
    return n < 0 || (size_t)n >= size ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

static esp_err_t audio_tone(const char *params_json, char *out, size_t size)
{
    cJSON *params = cJSON_Parse(params_json ? params_json : "{}");
    cJSON *freq = params ? cJSON_GetObjectItem(params, "frequencyHz") : NULL;
    cJSON *duration = params ? cJSON_GetObjectItem(params, "durationMs") : NULL;
    void *device = NULL;
    esp_codec_dev_sample_info_t fs = {.sample_rate = 16000, .channel = 2, .bits_per_sample = 16};
    esp_codec_dev_handle_t codec = NULL;
    esp_err_t err = ESP_OK;
    if (!cJSON_IsNumber(freq) || !cJSON_IsNumber(duration) || freq->valuedouble < 100 ||
        freq->valuedouble > 4000 || duration->valuedouble < 1 || duration->valuedouble > 2000 ||
        freq->valuedouble != (int)freq->valuedouble || duration->valuedouble != (int)duration->valuedouble) {
        cJSON_Delete(params); return ESP_ERR_INVALID_ARG;
    }
    err = esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_AUDIO_DAC, &device);
    if (err == ESP_OK && device) {
        codec = *(esp_codec_dev_handle_t *)device;
        if (esp_codec_dev_open(codec, &fs) != ESP_CODEC_DEV_OK) err = ESP_FAIL;
    } else err = ESP_FAIL;
    if (err == ESP_OK) {
        uint32_t frames = (uint32_t)(16000U * (uint32_t)duration->valuedouble / 1000U);
        int16_t samples[512]; float phase = 0.0f;
        float step = 2.0f * (float)M_PI * (float)freq->valuedouble / 16000.0f;
        uint32_t done = 0;
        while (done < frames && err == ESP_OK) {
            uint32_t n = frames - done > 256 ? 256 : frames - done;
            for (uint32_t i = 0; i < n; ++i) { samples[2*i] = samples[2*i+1] = (int16_t)(sinf(phase) * 9000.0f); phase += step; }
            if (esp_codec_dev_write(codec, samples, (int)(n * 2 * sizeof(int16_t))) != ESP_CODEC_DEV_OK) err = ESP_FAIL;
            done += n;
        }
        esp_codec_dev_close(codec);
    }
    cJSON_Delete(params);
    if (err != ESP_OK) return err;
    int n = snprintf(out, size, "{\"command\":\"audio.tone\",\"played\":true}");
    return n < 0 || (size_t)n >= size ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

static esp_err_t screen_clear(char *out, size_t size)
{
    esp_err_t err = system_ui_show_home();
    if (err != ESP_OK) return err;
    int n = snprintf(out, size, "{\"command\":\"device.screen.clear\",\"cleared\":true}");
    return n < 0 || (size_t)n >= size ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

/* Formats the coverage fragment for a text command response.
 *
 * Text is queued asynchronously, so "shown": true only means the request was
 * accepted - it cannot distinguish a real render from a row of missing-glyph
 * boxes. Reporting how many characters have no glyph closes that gap. The field
 * is null rather than 0 when the font is not loaded yet, so an absent answer is
 * never mistaken for a clean one. */
static void write_coverage_field(const char *text, char *out, size_t size)
{
    size_t missing = 0;
    uint32_t first = 0;
    if (system_ui_text_coverage(text, &missing, &first) != ESP_OK) {
        snprintf(out, size, "\"missing_glyphs\":null");
        return;
    }
    if (missing == 0) {
        snprintf(out, size, "\"missing_glyphs\":0");
        return;
    }
    snprintf(out, size, "\"missing_glyphs\":%u,\"first_missing\":\"U+%04X\"",
             (unsigned)missing, (unsigned)first);
}

static esp_err_t screen_text(const char *params_json, char *out, size_t size)
{
    cJSON *params = cJSON_Parse(params_json ? params_json : "{}");
    cJSON *text_item = params ? cJSON_GetObjectItem(params, "text") : NULL;
    const char *text = cJSON_GetStringValue(text_item);
    esp_err_t err;
    if (!text || !cJSON_IsString(text_item)) {
        cJSON_Delete(params);
        return ESP_ERR_INVALID_ARG;
    }
    /* A too-long payload is reported apart from a malformed one so the Gateway
       can tell "shrink the text" from "fix the request". */
    if (strlen(text) > SYSTEM_UI_SCREEN_TEXT_MAX) {
        cJSON_Delete(params);
        return ESP_ERR_INVALID_SIZE;
    }
    /* text points into the parsed tree, so this has to run before the delete. */
    char coverage[64];
    write_coverage_field(text, coverage, sizeof(coverage));
    err = system_ui_show_text(text);
    cJSON_Delete(params);
    if (err != ESP_OK) return err;
    int n = snprintf(out, size, "{\"command\":\"device.screen.text\",\"shown\":true,%s}", coverage);
    return n < 0 || (size_t)n >= size ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

static esp_err_t screen_fullscreen(const char *command, const char *params_json, char *out, size_t size)
{
    esp_err_t err;
    cJSON *params = NULL;
    /* Overwritten with the real answer in the text branch. The enter/clear/exit
       commands carry no text, so null is the honest value there. */
    char coverage[64] = "\"missing_glyphs\":null";
    if (strcmp(command, OPENCLAW_NODE_DEVICE_COMMAND_SCREEN_FULLSCREEN_ENTER) == 0) {
        err = system_ui_fullscreen_enter();
    } else if (strcmp(command, OPENCLAW_NODE_DEVICE_COMMAND_SCREEN_FULLSCREEN_CLEAR) == 0) {
        err = system_ui_fullscreen_clear();
    } else if (strcmp(command, OPENCLAW_NODE_DEVICE_COMMAND_SCREEN_FULLSCREEN_EXIT) == 0) {
        err = system_ui_fullscreen_exit();
    } else {
        params = cJSON_Parse(params_json ? params_json : "{}");
        cJSON *item = params ? cJSON_GetObjectItem(params, "text") : NULL;
        cJSON *orientation_item = params ? cJSON_GetObjectItem(params, "orientation") : NULL;
        const char *text = cJSON_GetStringValue(item);
        const char *orientation = cJSON_GetStringValue(orientation_item);
        if (!text || !cJSON_IsString(item)) {
            cJSON_Delete(params);
            return ESP_ERR_INVALID_ARG;
        }
        /* Case-insensitive, matching system_ui's own check: an orientation that
           only differs in case must not pass there and fail here. An empty
           string is documented and implemented as "portrait", so only a
           non-empty unknown value is rejected. */
        if (orientation && orientation[0] != '\0' &&
            strcasecmp(orientation, "portrait") != 0 &&
            strcasecmp(orientation, "landscape") != 0) {
            cJSON_Delete(params);
            return ESP_ERR_INVALID_ARG;
        }
        if (strlen(text) > SYSTEM_UI_SCREEN_TEXT_MAX) {
            cJSON_Delete(params);
            return ESP_ERR_INVALID_SIZE;
        }
        /* text points into the parsed tree, so this has to run before the delete. */
        write_coverage_field(text, coverage, sizeof(coverage));
        /* "landscape" is honoured: the UI rotates the fullscreen label as an
           object-level transform, so no display-level rotation is involved and
           the SPI flush geometry stays untouched. */
        err = system_ui_fullscreen_text(text, orientation ? orientation : "portrait");
    }
    cJSON_Delete(params);
    if (err != ESP_OK) return err;
    int n = snprintf(out, size, "{\"command\":\"%s\",\"accepted\":true,%s}", command, coverage);
    return n < 0 || (size_t)n >= size ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

esp_err_t openclaw_node_device_command(const char *command,
                                       const char *params_json,
                                       char *result_json,
                                       size_t result_size,
                                       void *user_ctx)
{
    (void)user_ctx;
    if (!command || !result_json || result_size == 0) return ESP_ERR_INVALID_ARG;
    if (strcmp(command, OPENCLAW_NODE_DEVICE_COMMAND_INFO) == 0) return write_info(result_json, result_size);
    if (strcmp(command, OPENCLAW_NODE_DEVICE_COMMAND_STATUS) == 0) return write_status(result_json, result_size);
    if (strcmp(command, OPENCLAW_NODE_DEVICE_COMMAND_NETWORK) == 0) return write_network(result_json, result_size);
    if (strcmp(command, OPENCLAW_NODE_DEVICE_COMMAND_BACKLIGHT) == 0) return set_backlight(params_json, result_json, result_size);
    if (strcmp(command, OPENCLAW_NODE_DEVICE_COMMAND_RESTART) == 0) {
        if (xTaskCreate(restart_task, "node_restart", 2048, NULL, 5, NULL) != pdPASS) return ESP_ERR_NO_MEM;
        snprintf(result_json, result_size, "{\"command\":\"device.restart\",\"scheduled\":true}");
        return ESP_OK;
    }
    if (strcmp(command, OPENCLAW_NODE_DEVICE_COMMAND_BUTTON_STATUS) == 0) return button_status(result_json, result_size);
    if (strcmp(command, OPENCLAW_NODE_DEVICE_COMMAND_AUDIO_VOLUME) == 0) return audio_volume(params_json, result_json, result_size);
    if (strcmp(command, OPENCLAW_NODE_DEVICE_COMMAND_AUDIO_TONE) == 0) return audio_tone(params_json, result_json, result_size);
    if (strcmp(command, OPENCLAW_NODE_DEVICE_COMMAND_SCREEN_CLEAR) == 0) return screen_clear(result_json, result_size);
    if (strcmp(command, OPENCLAW_NODE_DEVICE_COMMAND_SCREEN_TEXT) == 0) return screen_text(params_json, result_json, result_size);
    if (strcmp(command, OPENCLAW_NODE_DEVICE_COMMAND_SCREEN_FULLSCREEN_ENTER) == 0 ||
        strcmp(command, OPENCLAW_NODE_DEVICE_COMMAND_SCREEN_FULLSCREEN_TEXT) == 0 ||
        strcmp(command, OPENCLAW_NODE_DEVICE_COMMAND_SCREEN_FULLSCREEN_CLEAR) == 0 ||
        strcmp(command, OPENCLAW_NODE_DEVICE_COMMAND_SCREEN_FULLSCREEN_EXIT) == 0) {
        return screen_fullscreen(command, params_json, result_json, result_size);
    }
    const char *cap = map_native_cap(command);
    if (cap) return call_existing_cap(cap, params_json, result_json, result_size);
    return ESP_ERR_NOT_FOUND;
}
