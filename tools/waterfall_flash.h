/**
 * @file waterfall_flash.h
 * @brief Compartmentalized flash/marker display system for waterfall
 *
 * Provides a reusable pattern for detector visualization:
 *   - Waterfall band markers (colored bands at frequency positions)
 *   - Bar panel flashes (full-height colored bars)
 *
 * Each detector registers itself with visual properties and callbacks.
 * The render functions iterate through all registered sources.
 *
 * Usage:
 *   1. Call flash_source_register() for each detector at startup
 *   2. Call flash_draw_waterfall_bands() after drawing waterfall row
 *   3. Call flash_get_bar_override() when drawing each bar
 *   4. Call flash_decrement_all() at end of frame
 */

#ifndef WATERFALL_FLASH_H
#define WATERFALL_FLASH_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Configuration
 *============================================================================*/

#define FLASH_MAX_SOURCES   16      /* Maximum registered flash sources */

/*============================================================================
 * Flash Source Definition
 *============================================================================*/

/**
 * Flash source - represents one detector's visual feedback
 */
typedef struct {
    const char *name;               /* Display name ("tick", "marker", etc.) */

    /* Callbacks to detector (required) */
    int (*get_flash_frames)(void *ctx);     /* Returns frames remaining, 0 = not flashing */
    void (*decrement_flash)(void *ctx);     /* Decrement flash counter */
    void *ctx;                              /* Detector instance pointer */

    /* Waterfall band properties */
    int freq_hz;                    /* Center frequency for band marker */
    int band_half_width;            /* Half-width in pixels (e.g., 3 = 7px total) */
    uint8_t band_r, band_g, band_b; /* Band color */

    /* Bar panel properties */
    int bar_index;                  /* Which bucket bar to flash (-1 = none) */
    uint8_t bar_r, bar_g, bar_b;    /* Bar flash color */

} flash_source_t;

/*============================================================================
 * Registration API
 *============================================================================*/

/**
 * Initialize flash system (call once at startup)
 */
void flash_init(void);

/**
 * Register a flash source
 * @param source  Pointer to flash source definition (copied internally)
 * @return        Index of registered source, or -1 on failure
 */
int flash_register(const flash_source_t *source);

/**
 * Get number of registered sources
 */
int flash_get_count(void);

/**
 * Get registered source by index (for iteration)
 */
const flash_source_t *flash_get_source(int index);

/*============================================================================
 * Rendering API
 *============================================================================*/

/**
 * Draw waterfall band markers for all active flash sources
 *
 * Call after drawing the waterfall row pixels.
 *
 * @param pixels          Pixel buffer (RGB24)
 * @param row_offset      Byte offset to start of row (usually 0 for top row)
 * @param waterfall_width Width of waterfall area in pixels
 * @param window_width    Total window width (for row stride)
 * @param zoom_max_hz     Maximum frequency displayed (e.g., 5000 for ±5kHz)
 */
void flash_draw_waterfall_bands(uint8_t *pixels, int row_offset,
                                int waterfall_width, int window_width,
                                float zoom_max_hz);

/**
 * Check if a bar should be overridden with flash color
 *
 * Call when drawing each bucket bar. If returns true, use the output
 * color and set bar to full height.
 *
 * @param bar_index  Index of bar being drawn
 * @param r, g, b    Output: flash color if active
 * @return           true if bar should flash, false for normal drawing
 */
bool flash_get_bar_override(int bar_index, uint8_t *r, uint8_t *g, uint8_t *b);

/**
 * Decrement flash counters for all sources
 *
 * Call once per display frame, after all rendering.
 */
void flash_decrement_all(void);

/*============================================================================
 * Utility Macros for Common Patterns
 *============================================================================*/

/**
 * Helper to create flash source for tick-style detector (short pulse)
 */
#define FLASH_SOURCE_TICK(name_str, detector, get_fn, dec_fn, freq) \
    (flash_source_t){ \
        .name = name_str, \
        .get_flash_frames = (int (*)(void*))get_fn, \
        .decrement_flash = (void (*)(void*))dec_fn, \
        .ctx = detector, \
        .freq_hz = freq, \
        .band_half_width = 3, \
        .band_r = 180, .band_g = 0, .band_b = 255, \
        .bar_index = 4, \
        .bar_r = 180, .bar_g = 0, .bar_b = 255 \
    }

/**
 * Helper to create flash source for marker-style detector (long pulse)
 */
#define FLASH_SOURCE_MARKER(name_str, detector, get_fn, dec_fn, freq) \
    (flash_source_t){ \
        .name = name_str, \
        .get_flash_frames = (int (*)(void*))get_fn, \
        .decrement_flash = (void (*)(void*))dec_fn, \
        .ctx = detector, \
        .freq_hz = freq, \
        .band_half_width = 8, \
        .band_r = 255, .band_g = 50, .band_b = 50, \
        .bar_index = 4, \
        .bar_r = 255, .bar_g = 50, .bar_b = 50 \
    }

#ifdef __cplusplus
}
#endif

#endif /* WATERFALL_FLASH_H */
