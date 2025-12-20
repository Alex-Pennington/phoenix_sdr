/**
 * @file marker_detector.h
 * @brief WWV minute marker detector module
 *
 * Detects 800ms pulses at 1000Hz that occur at second 0 of each minute.
 * Uses sliding window accumulator with self-tracking baseline.
 *
 * DESIGN NOTES:
 *   - Self-contained detector with own FFT (50kHz, 256-pt)
 *   - Self-tracking baseline adapts slowly during IDLE state
 *   - Do NOT inject external baselines - incompatible scaling between FFT paths
 *   - Proven reliable in v133 testing
 */

#ifndef MARKER_DETECTOR_H
#define MARKER_DETECTOR_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Configuration - Tune these for minute marker signal
 *============================================================================*/

#define MARKER_SAMPLE_RATE      50000       /* Input sample rate (2MHz/40) */
#define MARKER_TARGET_FREQ_HZ   1000        /* Frequency bucket to watch */
#define MARKER_BANDWIDTH_HZ     200         /* Width of detection bucket */

/* Sliding window for 800ms pulse detection */
#define MARKER_WINDOW_MS        1000.0f     /* 1 second window to catch 800ms pulse */
#define MARKER_PULSE_MS         800.0f      /* Expected marker duration */
#define MARKER_MIN_DURATION_MS  500.0f      /* Minimum to count as marker */

/* FFT configuration */
#define MARKER_FFT_SIZE         256
#define MARKER_FRAME_MS         ((float)MARKER_FFT_SIZE * 1000.0f / MARKER_SAMPLE_RATE)  /* 5.12ms */
#define MARKER_WINDOW_FRAMES    ((int)(MARKER_WINDOW_MS / MARKER_FRAME_MS))  /* ~195 frames */

/*============================================================================
 * Detector State (opaque)
 *============================================================================*/

typedef struct marker_detector marker_detector_t;

/*============================================================================
 * Callback for marker events
 *============================================================================*/

typedef struct {
    int marker_number;
    float timestamp_ms;
    float since_last_marker_sec;
    float accumulated_energy;
    float peak_energy;
    float duration_ms;
} marker_event_t;

typedef void (*marker_callback_fn)(const marker_event_t *event, void *user_data);

/*============================================================================
 * Public API
 *============================================================================*/

/**
 * Create a new marker detector instance
 * @param csv_path  Path for CSV log file (NULL to disable logging)
 * @return          Detector instance or NULL on failure
 */
marker_detector_t *marker_detector_create(const char *csv_path);

/**
 * Destroy a marker detector instance
 */
void marker_detector_destroy(marker_detector_t *md);

/**
 * Set callback for marker events
 */
void marker_detector_set_callback(marker_detector_t *md, marker_callback_fn callback, void *user_data);

/**
 * Feed I/Q samples to detector
 * @return true if a marker was detected this sample
 */
bool marker_detector_process_sample(marker_detector_t *md, float i_sample, float q_sample);

/**
 * UI flash state
 */
int marker_detector_get_flash_frames(marker_detector_t *md);
void marker_detector_decrement_flash(marker_detector_t *md);

/**
 * Enable/disable detection
 */
void marker_detector_set_enabled(marker_detector_t *md, bool enabled);
bool marker_detector_get_enabled(marker_detector_t *md);

/**
 * Get current state for display
 */
float marker_detector_get_accumulated_energy(marker_detector_t *md);
float marker_detector_get_threshold(marker_detector_t *md);
float marker_detector_get_current_energy(marker_detector_t *md);
int marker_detector_get_marker_count(marker_detector_t *md);

/**
 * Print statistics to stdout
 */
void marker_detector_print_stats(marker_detector_t *md);

/**
 * Log metadata to CSV
 */
void marker_detector_log_metadata(marker_detector_t *md, uint64_t center_freq,
                                  uint32_t sample_rate, uint32_t gain_reduction,
                                  uint32_t lna_state);
void marker_detector_log_display_gain(marker_detector_t *md, float display_gain);

/**
 * Get timing info
 */
float marker_detector_get_frame_duration_ms(void);

/*============================================================================
 * Runtime Parameter Tuning
 *============================================================================*/

/* Threshold multiplier (2.0-5.0, default 3.0) - detection sensitivity */
void marker_detector_set_threshold_mult(marker_detector_t *md, float mult);
float marker_detector_get_threshold_mult(marker_detector_t *md);

/* Noise adaptation rate (0.0001-0.01, default 0.001) - baseline tracking speed */
void marker_detector_set_noise_adapt_rate(marker_detector_t *md, float rate);
float marker_detector_get_noise_adapt_rate(marker_detector_t *md);

/* Minimum duration (300.0-700.0, default 500.0) - pulse width threshold */
void marker_detector_set_min_duration_ms(marker_detector_t *md, float ms);
float marker_detector_get_min_duration_ms(marker_detector_t *md);

#ifdef __cplusplus
}
#endif

#endif /* MARKER_DETECTOR_H */
