/**
 * @file wwv_detector_manager.h
 * @brief Centralized WWV detector orchestration
 *
 * This module owns and coordinates all WWV signal detectors, providing:
 *   - Single point of detector creation/destruction
 *   - Explicit data flow between detectors
 *   - Clear documentation of what feeds what
 *   - Isolation from waterfall.c display/TCP code
 *
 * ARCHITECTURE:
 *
 *   I/Q Samples (from waterfall.c)
 *        │
 *        ├── DETECTOR PATH (50 kHz) ──► tick_detector ──► tick_correlator
 *        │                         └──► marker_detector ──► marker_correlator
 *        │
 *        └── DISPLAY PATH (12 kHz) ──► tone_tracker x3 (carrier, 500Hz, 600Hz)
 *                                 └──► slow_marker_detector (verification only)
 *
 * IMPORTANT DESIGN RULES:
 *   1. Detectors are SELF-CONTAINED - no cross-path baseline sharing
 *   2. Energy values from different FFT paths are NOT comparable
 *   3. All detector callbacks are handled internally, then forwarded
 *   4. waterfall.c only needs to call process_detector_sample() and process_display_sample()
 */

#ifndef WWV_DETECTOR_MANAGER_H
#define WWV_DETECTOR_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "kiss_fft.h"

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Opaque handle
 *============================================================================*/

typedef struct wwv_detector_manager wwv_detector_manager_t;

/*============================================================================
 * Configuration
 *============================================================================*/

typedef struct {
    const char *output_dir;         /* Directory for CSV logs (e.g., ".") */
    bool enable_tick_detector;
    bool enable_marker_detector;
    bool enable_sync_detector;
    bool enable_tone_trackers;
    bool enable_correlators;
    bool enable_slow_marker;        /* Display-path marker verification */
} wwv_detector_config_t;

/* Default config - all enabled */
#define WWV_DETECTOR_CONFIG_DEFAULT { \
    .output_dir = ".", \
    .enable_tick_detector = true, \
    .enable_marker_detector = true, \
    .enable_sync_detector = true, \
    .enable_tone_trackers = true, \
    .enable_correlators = true, \
    .enable_slow_marker = true \
}

/*============================================================================
 * Callbacks for external notification
 *============================================================================*/

/* Tick detected */
typedef struct {
    int tick_number;
    float timestamp_ms;
    float duration_ms;
    float energy;
} wwv_tick_event_t;

typedef void (*wwv_tick_callback_fn)(const wwv_tick_event_t *event, void *user_data);

/* Minute marker detected */
typedef struct {
    int marker_number;
    float timestamp_ms;
    float since_last_sec;
    float duration_ms;
    float energy;
} wwv_marker_event_t;

typedef void (*wwv_marker_callback_fn)(const wwv_marker_event_t *event, void *user_data);

/* Sync status update */
typedef struct {
    bool is_synced;
    int confidence;             /* 0-100 */
    float drift_ppm;
    int tick_count;
    int marker_count;
} wwv_sync_status_t;

typedef void (*wwv_sync_callback_fn)(const wwv_sync_status_t *status, void *user_data);

/*============================================================================
 * Lifecycle
 *============================================================================*/

/**
 * Create detector manager with given configuration
 */
wwv_detector_manager_t *wwv_detector_manager_create(const wwv_detector_config_t *config);

/**
 * Destroy detector manager and all owned detectors
 */
void wwv_detector_manager_destroy(wwv_detector_manager_t *mgr);

/*============================================================================
 * Sample Processing
 *============================================================================*/

/**
 * Process detector-path I/Q sample (50 kHz)
 * Feeds: tick_detector, marker_detector
 */
void wwv_detector_manager_process_detector_sample(wwv_detector_manager_t *mgr,
                                                   float i_sample, float q_sample);

/**
 * Process display-path I/Q sample (12 kHz)
 * Feeds: tone_trackers
 */
void wwv_detector_manager_process_display_sample(wwv_detector_manager_t *mgr,
                                                  float i_sample, float q_sample);

/**
 * Process display-path FFT output (for slow marker detector)
 * Called after waterfall's display FFT completes
 */
void wwv_detector_manager_process_display_fft(wwv_detector_manager_t *mgr,
                                               const kiss_fft_cpx *fft_out,
                                               float timestamp_ms);

/*============================================================================
 * Callbacks
 *============================================================================*/

void wwv_detector_manager_set_tick_callback(wwv_detector_manager_t *mgr,
                                             wwv_tick_callback_fn cb, void *user_data);

void wwv_detector_manager_set_marker_callback(wwv_detector_manager_t *mgr,
                                               wwv_marker_callback_fn cb, void *user_data);

void wwv_detector_manager_set_sync_callback(wwv_detector_manager_t *mgr,
                                             wwv_sync_callback_fn cb, void *user_data);

/*============================================================================
 * Status / Diagnostics
 *============================================================================*/

/**
 * Get current sync status
 */
wwv_sync_status_t wwv_detector_manager_get_sync_status(wwv_detector_manager_t *mgr);

/**
 * Get detector counts for display
 */
int wwv_detector_manager_get_tick_count(wwv_detector_manager_t *mgr);
int wwv_detector_manager_get_marker_count(wwv_detector_manager_t *mgr);

/**
 * Get flash frames for UI (tick and marker combined)
 */
int wwv_detector_manager_get_tick_flash(wwv_detector_manager_t *mgr);
int wwv_detector_manager_get_marker_flash(wwv_detector_manager_t *mgr);
void wwv_detector_manager_decrement_flash(wwv_detector_manager_t *mgr);

/**
 * Log metadata change (frequency, gain, etc.)
 */
void wwv_detector_manager_log_metadata(wwv_detector_manager_t *mgr,
                                        uint64_t center_freq,
                                        uint32_t sample_rate,
                                        uint32_t gain_reduction,
                                        uint32_t lna_state);

void wwv_detector_manager_log_display_gain(wwv_detector_manager_t *mgr, float display_gain);

/**
 * Print statistics for all detectors
 */
void wwv_detector_manager_print_stats(wwv_detector_manager_t *mgr);

#ifdef __cplusplus
}
#endif

#endif /* WWV_DETECTOR_MANAGER_H */
