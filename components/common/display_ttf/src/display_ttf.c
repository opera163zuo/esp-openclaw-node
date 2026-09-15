/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "display_ttf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "claw_paths.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "display_ttf";

#define DISPLAY_TTF_PATH_MAX 256

typedef struct {
    uint8_t size;
    lv_font_t *font;
    lv_font_t *fallback;
} display_ttf_font_slot_t;

typedef struct {
    SemaphoreHandle_t lock;
    uint8_t *font_data;
    size_t font_data_size;
    uint8_t *emoji_data;
    size_t emoji_data_size;
    display_ttf_font_slot_t slots[DISPLAY_TTF_MAX_FONTS];
    size_t slot_count;
    bool configured;
    char font_path[DISPLAY_TTF_PATH_MAX];
    char emoji_path[DISPLAY_TTF_PATH_MAX];
} display_ttf_state_t;

static display_ttf_state_t s_ttf;

/* Caller must hold s_ttf.lock. */
static void display_ttf_destroy_slot_locked(display_ttf_font_slot_t *slot)
{
    if (slot == NULL) {
        return;
    }
    if (slot->font) {
        lv_tiny_ttf_destroy(slot->font);
        slot->font = NULL;
    }
    if (slot->fallback) {
        lv_tiny_ttf_destroy(slot->fallback);
        slot->fallback = NULL;
    }
    slot->size = 0;
}

/* Caller must hold s_ttf.lock. */
static void display_ttf_destroy_fonts_locked(void)
{
    for (size_t i = 0; i < s_ttf.slot_count; ++i) {
        display_ttf_destroy_slot_locked(&s_ttf.slots[i]);
    }
    s_ttf.slot_count = 0;
}

static esp_err_t display_ttf_read_file(const char *vfs_path, uint8_t **out_data, size_t *out_size)
{
    FILE *fp;
    long file_size;
    uint8_t *data;
    size_t read_len;

    ESP_RETURN_ON_FALSE(vfs_path && out_data && out_size, ESP_ERR_INVALID_ARG, TAG, "font read argument missing");
    fp = fopen(vfs_path, "rb");
    if (!fp) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return ESP_FAIL;
    }
    file_size = ftell(fp);
    if (file_size <= 0) {
        fclose(fp);
        ESP_LOGW(TAG, "font file is empty: %s", vfs_path);
        return ESP_ERR_INVALID_SIZE;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return ESP_FAIL;
    }

    data = (uint8_t *)heap_caps_malloc((size_t)file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) {
        data = (uint8_t *)heap_caps_malloc((size_t)file_size, MALLOC_CAP_8BIT);
    }
    if (!data) {
        fclose(fp);
        ESP_LOGW(TAG, "font allocation failed: %ld bytes", file_size);
        return ESP_ERR_NO_MEM;
    }

    read_len = fread(data, 1, (size_t)file_size, fp);
    fclose(fp);
    if (read_len != (size_t)file_size) {
        free(data);
        ESP_LOGW(TAG, "font read failed: %s, read=%lu size=%ld",
                 vfs_path, (unsigned long)read_len, file_size);
        return ESP_FAIL;
    }

    *out_data = data;
    *out_size = (size_t)file_size;
    return ESP_OK;
}

/* Resolve a font path under the DATA root first, then the SYSTEM root, matching
 * the lookup order used by the system UI. */
static esp_err_t display_ttf_load_font_data(const char *font_path, uint8_t **out_data, size_t *out_size)
{
    char vfs_path[DISPLAY_TTF_PATH_MAX] = {0};
    esp_err_t err;

    *out_data = NULL;
    *out_size = 0;

    err = claw_paths_join(CLAW_PATH_DATA, font_path, vfs_path, sizeof(vfs_path));
    if (err == ESP_OK) {
        err = display_ttf_read_file(vfs_path, out_data, out_size);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "font loaded from data root: %s (%lu bytes)",
                     font_path, (unsigned long)*out_size);
            return ESP_OK;
        }
        if (err != ESP_ERR_NOT_FOUND) {
            return err;
        }
    }

    err = claw_paths_join(CLAW_PATH_SYSTEM, font_path, vfs_path, sizeof(vfs_path));
    if (err == ESP_OK) {
        err = display_ttf_read_file(vfs_path, out_data, out_size);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "font loaded from system root: %s (%lu bytes)",
                     font_path, (unsigned long)*out_size);
            return ESP_OK;
        }
    }
    return err == ESP_OK ? ESP_ERR_NOT_FOUND : err;
}

static esp_err_t display_ttf_lock(void)
{
    ESP_RETURN_ON_FALSE(s_ttf.lock != NULL, ESP_ERR_INVALID_STATE, TAG, "display_ttf_configure() not called");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_ttf.lock, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "font lock timeout");
    return ESP_OK;
}

static void display_ttf_unlock(void)
{
    if (s_ttf.lock) {
        xSemaphoreGive(s_ttf.lock);
    }
}

static esp_err_t display_ttf_ensure_lock(void)
{
    if (s_ttf.lock == NULL) {
        s_ttf.lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_ttf.lock != NULL, ESP_ERR_NO_MEM, TAG, "create font mutex failed");
    }
    return ESP_OK;
}

esp_err_t display_ttf_configure(const char *font_path, const char *emoji_path)
{
    uint8_t *font_data = NULL;
    uint8_t *emoji_data = NULL;
    size_t font_data_size = 0;
    size_t emoji_data_size = 0;
    esp_err_t err;

    if (!font_path || !font_path[0]) {
        font_path = DISPLAY_TTF_DEFAULT_FONT_PATH;
    }
    if (!emoji_path || !emoji_path[0]) {
        emoji_path = DISPLAY_TTF_DEFAULT_EMOJI_PATH;
    }

    ESP_RETURN_ON_ERROR(display_ttf_ensure_lock(), TAG, "font mutex unavailable");
    ESP_RETURN_ON_ERROR(display_ttf_lock(), TAG, "font lock failed");

    /* Idempotent: several subsystems call this during bring-up, and rebuilding
     * the cache would pull fonts out from under whoever is already using them. */
    if (s_ttf.configured &&
            strcmp(s_ttf.font_path, font_path) == 0 &&
            strcmp(s_ttf.emoji_path, emoji_path) == 0) {
        display_ttf_unlock();
        return ESP_OK;
    }

    /* Switching assets: drop the previous generation first. */
    display_ttf_destroy_fonts_locked();
    free(s_ttf.font_data);
    s_ttf.font_data = NULL;
    s_ttf.font_data_size = 0;
    free(s_ttf.emoji_data);
    s_ttf.emoji_data = NULL;
    s_ttf.emoji_data_size = 0;
    s_ttf.configured = false;
    s_ttf.font_path[0] = '\0';
    s_ttf.emoji_path[0] = '\0';

    err = display_ttf_load_font_data(font_path, &font_data, &font_data_size);
    if (err != ESP_OK) {
        display_ttf_unlock();
        ESP_LOGE(TAG, "ui font unavailable: %s (%s)", font_path, esp_err_to_name(err));
        return err;
    }

    /* The emoji font is optional: without it text still renders, emoji simply
     * fall back to the missing-glyph placeholder. */
    err = display_ttf_load_font_data(emoji_path, &emoji_data, &emoji_data_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "emoji fallback font unavailable: %s (%s)", emoji_path, esp_err_to_name(err));
        emoji_data = NULL;
        emoji_data_size = 0;
    }

    s_ttf.font_data = font_data;
    s_ttf.font_data_size = font_data_size;
    s_ttf.emoji_data = emoji_data;
    s_ttf.emoji_data_size = emoji_data_size;
    snprintf(s_ttf.font_path, sizeof(s_ttf.font_path), "%s", font_path);
    snprintf(s_ttf.emoji_path, sizeof(s_ttf.emoji_path), "%s", emoji_path);
    s_ttf.configured = true;

    display_ttf_unlock();
    ESP_LOGI(TAG, "text backend ready: ui=%lu bytes emoji=%lu bytes",
             (unsigned long)font_data_size, (unsigned long)emoji_data_size);
    return ESP_OK;
}

void display_ttf_release(void)
{
    if (display_ttf_lock() != ESP_OK) {
        return;
    }
    display_ttf_destroy_fonts_locked();
    free(s_ttf.font_data);
    s_ttf.font_data = NULL;
    s_ttf.font_data_size = 0;
    free(s_ttf.emoji_data);
    s_ttf.emoji_data = NULL;
    s_ttf.emoji_data_size = 0;
    s_ttf.configured = false;
    s_ttf.font_path[0] = '\0';
    s_ttf.emoji_path[0] = '\0';
    display_ttf_unlock();
}

const lv_font_t *display_ttf_font(uint8_t size)
{
    display_ttf_font_slot_t *slot = NULL;
    lv_font_t *font;
    uint8_t requested = size;

    if (requested == 0) {
        requested = DISPLAY_TTF_DEFAULT_FONT_SIZE;
    }
    if (requested > DISPLAY_TTF_MAX_FONT_SIZE) {
        ESP_LOGW(TAG, "font size %u clamped to %u", requested, DISPLAY_TTF_MAX_FONT_SIZE);
        requested = DISPLAY_TTF_MAX_FONT_SIZE;
    }

    if (display_ttf_lock() != ESP_OK) {
        return NULL;
    }
    if (!s_ttf.configured) {
        display_ttf_unlock();
        ESP_LOGW(TAG, "font requested before configuration");
        return NULL;
    }

    for (size_t i = 0; i < s_ttf.slot_count; ++i) {
        if (s_ttf.slots[i].size == requested) {
            font = s_ttf.slots[i].font;
            display_ttf_unlock();
            return font;
        }
    }

    if (s_ttf.slot_count >= DISPLAY_TTF_MAX_FONTS) {
        ESP_LOGW(TAG, "font cache full (%d sizes)", DISPLAY_TTF_MAX_FONTS);
        goto unlock;
    }

    slot = &s_ttf.slots[s_ttf.slot_count];
    slot->font = lv_tiny_ttf_create_data_ex(s_ttf.font_data, s_ttf.font_data_size, requested,
                                            LV_FONT_KERNING_NORMAL, DISPLAY_TTF_CACHE_GLYPH_CNT);
    if (slot->font == NULL) {
        ESP_LOGE(TAG, "create ui font failed: size=%u", requested);
        goto unlock;
    }

    if (s_ttf.emoji_data) {
        slot->fallback = lv_tiny_ttf_create_data_ex(s_ttf.emoji_data, s_ttf.emoji_data_size, requested,
                                                    LV_FONT_KERNING_NORMAL, DISPLAY_TTF_CACHE_GLYPH_CNT);
        if (slot->fallback) {
            /* lv_font_get_glyph_dsc() walks this chain, so emoji resolve to real
             * glyphs instead of the missing-glyph placeholder. */
            slot->font->fallback = slot->fallback;
        } else {
            ESP_LOGW(TAG, "create emoji fallback font failed: size=%u", requested);
        }
    }

    slot->size = requested;
    s_ttf.slot_count++;
    ESP_LOGI(TAG, "font created: size=%u fallback=%s", requested, slot->fallback ? "yes" : "no");
    font = slot->font;
    display_ttf_unlock();
    return font;

unlock:
    display_ttf_destroy_slot_locked(slot);
    display_ttf_unlock();
    return NULL;
}

size_t display_ttf_utf8_decode(const char *text, uint32_t *out_codepoint)
{
    const uint8_t *s = (const uint8_t *)text;
    uint8_t b0;

    if (!text || !out_codepoint) {
        return 0;
    }
    b0 = s[0];
    if (b0 == '\0') {
        return 0;
    }
    if (b0 < 0x80) {
        *out_codepoint = b0;
        return 1;
    }
    if ((b0 & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        uint32_t cp = ((uint32_t)(b0 & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
        if (cp >= 0x80) {
            *out_codepoint = cp;
            return 2;
        }
        return 0;
    }
    if ((b0 & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        uint32_t cp = ((uint32_t)(b0 & 0x0F) << 12) |
                      ((uint32_t)(s[1] & 0x3F) << 6) |
                      (uint32_t)(s[2] & 0x3F);
        if (cp >= 0x800) {
            *out_codepoint = cp;
            return 3;
        }
        return 0;
    }
    if ((b0 & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 &&
            (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        uint32_t cp = ((uint32_t)(b0 & 0x07) << 18) |
                      ((uint32_t)(s[1] & 0x3F) << 12) |
                      ((uint32_t)(s[2] & 0x3F) << 6) |
                      (uint32_t)(s[3] & 0x3F);
        if (cp >= 0x10000 && cp <= 0x10FFFF) {
            *out_codepoint = cp;
            return 4;
        }
        return 0;
    }
    return 0;
}

/* Measure one line bounded by @p end (exclusive) or the NUL terminator. */
static int32_t display_ttf_measure_line(const lv_font_t *font, const char *text, const char *end)
{
    const char *p = text;
    int32_t width = 0;

    while (p < end && *p != '\0') {
        uint32_t codepoint = 0;
        uint32_t next_codepoint = 0;
        size_t consumed = display_ttf_utf8_decode(p, &codepoint);

        if (consumed == 0) {
            /* Invalid byte: skip it so measurement never stalls. */
            p++;
            continue;
        }
        if (p + consumed < end && *(p + consumed) != '\0') {
            (void)display_ttf_utf8_decode(p + consumed, &next_codepoint);
        }
        width += lv_font_get_glyph_width(font, codepoint, next_codepoint);
        p += consumed;
    }
    return width;
}

esp_err_t display_ttf_measure_font(const lv_font_t *font, const char *text,
                                   int32_t *out_width, int32_t *out_height)
{
    int32_t max_width = 0;
    int32_t lines = 1;
    const char *line_start;

    if (out_width) {
        *out_width = 0;
    }
    if (out_height) {
        *out_height = 0;
    }
    ESP_RETURN_ON_FALSE(text != NULL, ESP_ERR_INVALID_ARG, TAG, "text is NULL");
    ESP_RETURN_ON_FALSE(font != NULL, ESP_ERR_INVALID_ARG, TAG, "font is NULL");

    line_start = text;
    for (const char *p = text;; ++p) {
        if (*p == '\n' || *p == '\0') {
            int32_t line_width = display_ttf_measure_line(font, line_start, p);
            if (line_width > max_width) {
                max_width = line_width;
            }
            if (*p == '\0') {
                break;
            }
            lines++;
            line_start = p + 1;
        }
    }

    if (out_width) {
        *out_width = max_width;
    }
    if (out_height) {
        *out_height = lines * font->line_height;
    }
    return ESP_OK;
}

esp_err_t display_ttf_measure(const char *text, uint8_t size, int32_t *out_width, int32_t *out_height)
{
    const lv_font_t *font;

    ESP_RETURN_ON_FALSE(text != NULL, ESP_ERR_INVALID_ARG, TAG, "text is NULL");

    font = display_ttf_font(size);
    ESP_RETURN_ON_FALSE(font != NULL, ESP_ERR_NOT_SUPPORTED, TAG, "font size %u unavailable", size);

    return display_ttf_measure_font(font, text, out_width, out_height);
}

bool display_ttf_glyph_get(const lv_font_t *font, uint32_t codepoint, uint32_t next_codepoint,
                           display_ttf_glyph_t *out_glyph)
{
    const void *bitmap;
    const lv_draw_buf_t *buffer;
    bool found;

    if (!font || !out_glyph) {
        return false;
    }
    memset(out_glyph, 0, sizeof(*out_glyph));

    /* lv_font_get_glyph_dsc() resolves the fallback chain and reports the font
     * that actually supplied the glyph in dsc.resolved_font. */
    found = lv_font_get_glyph_dsc(font, &out_glyph->dsc, codepoint, next_codepoint);
    if (!found) {
        /* With LV_USE_FONT_PLACEHOLDER the descriptor still carries a box, but
         * there is no bitmap behind it. Report it so the caller can draw the
         * missing-glyph box instead of silently dropping the character. */
        if (out_glyph->dsc.is_placeholder &&
                out_glyph->dsc.box_w > 0 && out_glyph->dsc.box_h > 0) {
            out_glyph->is_placeholder = true;
            out_glyph->box_w = out_glyph->dsc.box_w;
            out_glyph->box_h = out_glyph->dsc.box_h;
            out_glyph->offset_x = out_glyph->dsc.ofs_x;
            out_glyph->offset_y = out_glyph->dsc.ofs_y;
            out_glyph->advance = out_glyph->dsc.adv_w;
            return true;
        }
        return false;
    }
    if (out_glyph->dsc.box_w == 0 || out_glyph->dsc.box_h == 0) {
        /* Whitespace and other zero-box glyphs only advance the pen. */
        out_glyph->advance = out_glyph->dsc.adv_w;
        return true;
    }
    /* lv_font_get_glyph_bitmap() dereferences dsc.resolved_font. */
    if (out_glyph->dsc.resolved_font == NULL) {
        ESP_LOGW(TAG, "glyph U+%04X has no resolved font", (unsigned)codepoint);
        goto reset;
    }

    /* tiny_ttf ignores the caller-provided buffer and hands back its own cached
     * A8 draw buffer, so no scratch buffer is needed here. */
    bitmap = lv_font_get_glyph_bitmap(&out_glyph->dsc, NULL);
    if (!bitmap) {
        lv_font_glyph_release_draw_data(&out_glyph->dsc);
        goto reset;
    }

    buffer = (const lv_draw_buf_t *)bitmap;
    out_glyph->coverage = buffer->data;
    out_glyph->box_w = (uint16_t)buffer->header.w;
    out_glyph->box_h = (uint16_t)buffer->header.h;
    out_glyph->stride = (uint16_t)buffer->header.stride;
    out_glyph->offset_x = out_glyph->dsc.ofs_x;
    out_glyph->offset_y = out_glyph->dsc.ofs_y;
    out_glyph->advance = out_glyph->dsc.adv_w;
    return true;

reset:
    memset(out_glyph, 0, sizeof(*out_glyph));
    return false;
}

void display_ttf_glyph_release(display_ttf_glyph_t *glyph)
{
    if (!glyph) {
        return;
    }
    lv_font_glyph_release_draw_data(&glyph->dsc);
    memset(glyph, 0, sizeof(*glyph));
}
