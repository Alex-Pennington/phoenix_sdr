/**
 * @file waterfall_flash.c
 * @brief Compartmentalized flash/marker display system implementation
 */

#include "waterfall_flash.h"
#include <string.h>
#include <stdio.h>

/*============================================================================
 * Internal State
 *============================================================================*/

static flash_source_t g_sources[FLASH_MAX_SOURCES];
static int g_source_count = 0;
static bool g_initialized = false;

/*============================================================================
 * Registration API Implementation
 *============================================================================*/

void flash_init(void) {
    memset(g_sources, 0, sizeof(g_sources));
    g_source_count = 0;
    g_initialized = true;
}

int flash_register(const flash_source_t *source) {
    if (!g_initialized) {
        flash_init();
    }

    if (!source || !source->get_flash_frames || !source->decrement_flash) {
        fprintf(stderr, "[FLASH] Invalid source registration (missing callbacks)\n");
        return -1;
    }

    if (g_source_count >= FLASH_MAX_SOURCES) {
        fprintf(stderr, "[FLASH] Maximum sources reached (%d)\n", FLASH_MAX_SOURCES);
        return -1;
    }

    /* Copy source definition */
    g_sources[g_source_count] = *source;

    printf("[FLASH] Registered '%s': freq=%dHz, bar=%d\n",
           source->name ? source->name : "(unnamed)",
           source->freq_hz,
           source->bar_index);

    return g_source_count++;
}

int flash_get_count(void) {
    return g_source_count;
}

const flash_source_t *flash_get_source(int index) {
    if (index < 0 || index >= g_source_count) {
        return NULL;
    }
    return &g_sources[index];
}

/*============================================================================
 * Rendering API Implementation
 *============================================================================*/

void flash_draw_waterfall_bands(uint8_t *pixels, int row_offset,
                                int waterfall_width, int window_width,
                                float zoom_max_hz) {
    if (!pixels || waterfall_width <= 0 || zoom_max_hz <= 0.0f) {
        return;
    }

    float pixels_per_hz = (waterfall_width / 2.0f) / zoom_max_hz;
    int center_x = waterfall_width / 2;

    for (int i = 0; i < g_source_count; i++) {
        flash_source_t *src = &g_sources[i];

        /* Check if this source is flashing */
        if (!src->ctx || src->get_flash_frames(src->ctx) <= 0) {
            continue;
        }

        /* Calculate frequency position */
        int offset = (int)(src->freq_hz * pixels_per_hz);
        int half_w = src->band_half_width;

        /* Draw at +freq position */
        for (int dx = -half_w; dx <= half_w; dx++) {
            int x = center_x + offset + dx;
            if (x >= 0 && x < waterfall_width) {
                int idx = row_offset + x * 3;
                pixels[idx + 0] = src->band_r;
                pixels[idx + 1] = src->band_g;
                pixels[idx + 2] = src->band_b;
            }
        }

        /* Draw at -freq position (mirror) */
        for (int dx = -half_w; dx <= half_w; dx++) {
            int x = center_x - offset + dx;
            if (x >= 0 && x < waterfall_width) {
                int idx = row_offset + x * 3;
                pixels[idx + 0] = src->band_r;
                pixels[idx + 1] = src->band_g;
                pixels[idx + 2] = src->band_b;
            }
        }
    }
}

bool flash_get_bar_override(int bar_index, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (bar_index < 0) {
        return false;
    }

    /* Check sources in reverse order (later registrations take precedence) */
    for (int i = g_source_count - 1; i >= 0; i--) {
        flash_source_t *src = &g_sources[i];

        if (src->bar_index != bar_index) {
            continue;
        }

        if (!src->ctx || src->get_flash_frames(src->ctx) <= 0) {
            continue;
        }

        /* This source is flashing on this bar */
        if (r) *r = src->bar_r;
        if (g) *g = src->bar_g;
        if (b) *b = src->bar_b;
        return true;
    }

    return false;
}

void flash_decrement_all(void) {
    for (int i = 0; i < g_source_count; i++) {
        flash_source_t *src = &g_sources[i];

        if (!src->ctx) {
            continue;
        }

        if (src->get_flash_frames(src->ctx) > 0) {
            src->decrement_flash(src->ctx);
        }
    }
}
