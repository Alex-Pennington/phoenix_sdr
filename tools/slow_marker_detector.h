/**
 * @file slow_marker_detector.h
 * @brief Slow-path marker detector using 12 kHz display stream
 *
 * High-resolution frequency analysis with ~10 frame integration.
 * Provides noise floor estimate to fast path.
 */

#ifndef SLOW_MARKER_DETECTOR_H
#define SLOW_MARKER_DETECTOR_H

#include <stdint.h>
#include <stdbool.h>
#include "kiss_fft.h"

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Configuration
 *============================================================================*/

#define SLOW_MARKER_SAMPLE_RATE     12000       /* Display stream rate */
#define SLOW_MARKER_FFT_SIZE        2048        /* 5.86 Hz/bin resolution */
#define SLOW_MARKER_TARGET_HZ       1000        /* Center frequency */
#define SLOW_MARKER_BANDWIDTH_HZ    100         /* Tight ±50 Hz bucket */
#define SLOW_MARKER_ACCUM_FRAMES    10          /* ~850ms integration */

/*============================================================================
 * Types
 *============================================================================*/

typedef struct slow_marker_detector slow_marker_detector_t;

typedef struct {
    float energy;           /* Accumulated 1000 Hz energy */
    float snr_db;           /* Signal-to-noise ratio */
    float noise_floor;      /* Current noise estimate */
    float timestamp_ms;     /* Frame timestamp */
    bool above_threshold;   /* Energy exceeds detection threshold */
} slow_marker_frame_t;

typedef void (*slow_marker_callback_fn)(const slow_marker_frame_t *frame, void *user_data);

/*============================================================================
 * API
 *============================================================================*/

slow_marker_detector_t *slow_marker_detector_create(void);
void slow_marker_detector_destroy(slow_marker_detector_t *smd);

/* Feed from display path (called every 85ms effective) */
void slow_marker_detector_process_fft(slow_marker_detector_t *smd,
                                       const kiss_fft_cpx *fft_out,
                                       float timestamp_ms);

void slow_marker_detector_set_callback(slow_marker_detector_t *smd,
                                        slow_marker_callback_fn cb, void *user_data);

/* Getters for fast path */
float slow_marker_detector_get_noise_floor(slow_marker_detector_t *smd);
float slow_marker_detector_get_current_energy(slow_marker_detector_t *smd);
bool slow_marker_detector_is_above_threshold(slow_marker_detector_t *smd);

#ifdef __cplusplus
}
#endif

#endif /* SLOW_MARKER_DETECTOR_H */
