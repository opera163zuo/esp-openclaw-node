/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

/* Shared TrueType text backend.
 *
 * This component owns the single source of truth for on-screen text: the
 * bundled Noto Sans SC subset plus an optional monochrome emoji font wired in
 * as the LVGL font fallback chain. Every text path in the firmware is expected
 * to go through it, so a character renders identically whether it was drawn by
 * the LVGL system UI, the Lua `lvgl` module, or the raw Lua `display` text APIs.
 *
 * It deliberately does not touch framebuffers: callers keep their own pixel
 * blitting so the LVGL renderer and the raw display HAL can each honour their
 * own clipping, dirty tracking and flush strategy. The component supplies the
 * font handles, text metrics, UTF-8 decoding and glyph coverage bitmaps.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Bundled UI font, resolved under the SYSTEM root first and the DATA root second. */
#define DISPLAY_TTF_DEFAULT_FONT_PATH "fonts/NotoSansSC-Regular-sub.ttf"
/** Optional monochrome emoji fallback font. Missing file disables fallback. */
#define DISPLAY_TTF_DEFAULT_EMOJI_PATH "fonts/NotoEmoji-Regular-sub.ttf"
#define DISPLAY_TTF_DEFAULT_FONT_SIZE 24
/** Upper bound for a requested pixel size; larger requests are clamped. */
#define DISPLAY_TTF_MAX_FONT_SIZE 128
/** Number of distinct pixel sizes cached at once. */
#define DISPLAY_TTF_MAX_FONTS 16
#define DISPLAY_TTF_CACHE_GLYPH_CNT 128

/**
 * @brief One rasterised glyph.
 *
 * @c coverage is an 8-bit alpha bitmap of @c box_w x @c box_h pixels with
 * @c stride bytes per row, valid until @ref display_ttf_glyph_release.
 */
typedef struct {
    lv_font_glyph_dsc_t dsc;    /*!< Backend handle; pass back to release */
    const uint8_t *coverage;    /*!< A8 coverage bitmap, NULL for a placeholder */
    uint16_t box_w;             /*!< Bitmap width in pixels */
    uint16_t box_h;             /*!< Bitmap height in pixels */
    uint16_t stride;            /*!< Bytes per bitmap row */
    int16_t offset_x;           /*!< Bitmap left edge relative to the pen */
    int16_t offset_y;           /*!< Bitmap bottom edge relative to the baseline */
    uint16_t advance;           /*!< Pen advance in pixels */
    bool is_placeholder;        /*!< Code point missing from every font */
} display_ttf_glyph_t;

/**
 * @brief Load the font data and reset the size cache.
 *
 * Passing NULL for a path selects the corresponding default. A NULL or
 * unreadable @p emoji_path simply leaves the fallback chain empty.
 *
 * @return ESP_OK on success, an error when the UI font cannot be loaded.
 */
esp_err_t display_ttf_configure(const char *font_path, const char *emoji_path);

/**
 * @brief Destroy every cached font and release the font data.
 *
 * Safe to call when nothing is configured. Must not be called while another
 * task holds a font handle from @ref display_ttf_font.
 */
void display_ttf_release(void);

/**
 * @brief Get the font for a pixel size, creating it on first use.
 *
 * @param size Requested pixel size; 0 selects @ref DISPLAY_TTF_DEFAULT_FONT_SIZE.
 * @return A font owned by this component, or NULL when unavailable.
 */
const lv_font_t *display_ttf_font(uint8_t size);

/**
 * @brief Measure a possibly multi-line string.
 *
 * Line breaks advance by the font line height; the reported width is the widest
 * line. Both outputs are optional.
 */
esp_err_t display_ttf_measure(const char *text, uint8_t size, int32_t *out_width, int32_t *out_height);

/**
 * @brief Measure with an already-resolved font handle.
 *
 * Equivalent to @ref display_ttf_measure but skips the size lookup, for callers
 * that already hold the font from @ref display_ttf_font.
 */
esp_err_t display_ttf_measure_font(const lv_font_t *font, const char *text,
                                   int32_t *out_width, int32_t *out_height);

/**
 * @brief Decode one UTF-8 sequence.
 *
 * @param text       Pointer into a NUL-terminated string.
 * @param out_codepoint Receives the decoded code point on success.
 * @return Bytes consumed, or 0 when the input is not a valid sequence.
 */
size_t display_ttf_utf8_decode(const char *text, uint32_t *out_codepoint);

/**
 * @brief Resolve a code point through the font fallback chain and rasterise it.
 *
 * @param font            Font from @ref display_ttf_font.
 * @param codepoint       Code point to draw.
 * @param next_codepoint  Following code point, used for kerning; 0 to skip.
 * @param out_glyph       Receives the glyph on success.
 * @return true when a glyph with a coverage bitmap is available.
 */
bool display_ttf_glyph_get(const lv_font_t *font, uint32_t codepoint, uint32_t next_codepoint,
                           display_ttf_glyph_t *out_glyph);

/**
 * @brief Release the glyph backend entry taken by @ref display_ttf_glyph_get.
 */
void display_ttf_glyph_release(display_ttf_glyph_t *glyph);

#ifdef __cplusplus
}
#endif
