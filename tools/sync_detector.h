/**
 * @file sync_detector.h
 * @brief WWV sync detector - correlates tick and marker detector inputs
 *
 * This detector receives events from both the tick detector (duration-based)
 * and marker detector (energy accumulation-based) and correlates them to
 * confirm minute markers with high confidence.
 *
 * State progression:
 *   ACQUIRING -> TENTATIVE -> LOCKED
 *
 * A confirmed marker requires both detectors to fire within a correlation
 * window. Once locked, the sync detector provides authoritative minute
 * marker timing for the rest of the system.
 */

#ifndef SYNC_DETECTOR_H
#define SYNC_DETECTOR_H

#include <stdint.h>
#include <stdbool.h>

/*============================================================================
 * Types
 *============================================================================*/

/* Sync confidence states */
typedef enum {
    SYNC_ACQUIRING,    /* No confirmed markers yet */
    SYNC_TENTATIVE,    /* 1 confirmed marker */
    SYNC_LOCKED        /* 2+ markers with ~60s intervals */
} sync_state_t;

/* Opaque handle */
typedef struct sync_detector sync_detector_t;

/*============================================================================
 * API
 *============================================================================*/

/**
 * Create sync detector
 * @param csv_path Path for CSV log file (NULL to disable logging)
 * @return Detector handle or NULL on failure
 */
sync_detector_t *sync_detector_create(const char *csv_path);

/**
 * Destroy sync detector and free resources
 */
void sync_detector_destroy(sync_detector_t *sd);

/**
 * Report minute marker from tick detector (duration-based detection)
 * @param sd Detector handle
 * @param timestamp_ms Timestamp when marker pulse ended
 * @param duration_ms Duration of the pulse (should be ~800ms)
 * @param corr_ratio Correlation ratio from tick detector
 */
void sync_detector_tick_marker(sync_detector_t *sd, float timestamp_ms, 
                                float duration_ms, float corr_ratio);

/**
 * Report minute marker from marker detector (energy accumulation-based)
 * @param sd Detector handle
 * @param timestamp_ms Timestamp of detection
 * @param accum_energy Accumulated energy in detection window
 * @param duration_ms Duration of accumulated energy above threshold
 */
void sync_detector_marker_event(sync_detector_t *sd, float timestamp_ms,
                                 float accum_energy, float duration_ms);

/**
 * Get current sync state
 */
sync_state_t sync_detector_get_state(sync_detector_t *sd);

/**
 * Get state as string for display
 */
const char *sync_state_name(sync_state_t state);

/**
 * Get timestamp of last confirmed marker
 */
float sync_detector_get_last_marker_ms(sync_detector_t *sd);

/**
 * Get count of confirmed markers
 */
int sync_detector_get_confirmed_count(sync_detector_t *sd);

/**
 * Get flash frames remaining (for UI feedback)
 */
int sync_detector_get_flash_frames(sync_detector_t *sd);

/**
 * Decrement flash counter (call each frame)
 */
void sync_detector_decrement_flash(sync_detector_t *sd);

#endif /* SYNC_DETECTOR_H */
