/**
 * @file bcd_freq_detector.h
 * @brief WWV BCD frequency-domain detector module
 *
 * Frequency-domain focused FFT for confident 100Hz presence detection.
 * Uses large FFT (2048) for fine frequency resolution at expense of time precision.
 *
 * Pattern: Follows marker_detector.h structure (sliding window accumulator)
 *
 * Design notes:
 *   - Self-contained detector with own FFT (50kHz, 2048-pt)
 *   - Own sample buffer and state machine
 *   - Sliding window accumulator for 800ms pulse detection
 *   - Self-tracking baseline (proven reliable approach)
 *   - Designed to run in parallel with bcd_time_detector
 *   - Works with bcd_correlator for dual-path symbol confirmation
 */

#ifndef BCD_FREQ_DETECTOR_H
#define BCD_FREQ_DETECTOR_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Configuration - Tune these for 100Hz BCD signal
 *============================================================================*/

#define BCD_FREQ_FFT_SIZE           2048    /* 40.96ms frames at 50kHz */
#define BCD_FREQ_SAMPLE_RATE        50000   /* Input sample rate (2MHz/40) */
#define BCD_FREQ_TARGET_FREQ_HZ     100     /* BCD subcarrier frequency */
#define BCD_FREQ_BANDWIDTH_HZ       15      /* Narrow bucket for precise isolation */

/* Sliding window for pulse detection */
#define BCD_FREQ_WINDOW_MS          1000.0f /* 1 second window */
#define BCD_FREQ_PULSE_MIN_MS       100.0f  /* Minimum valid pulse */
#define BCD_FREQ_PULSE_MAX_MS       1000.0f /* Maximum valid pulse */

/* Detection thresholds */
#define BCD_FREQ_THRESHOLD_MULT     3.0f    /* Accumulated must be 3x baseline */
#define BCD_FREQ_NOISE_ADAPT_RATE   0.001f  /* Slow baseline adaptation */

/*============================================================================
 * Detector State (opaque to caller)
 *============================================================================*/

typedef struct bcd_freq_detector bcd_freq_detector_t;

/*============================================================================
 * Callback for pulse events
 *============================================================================*/

typedef struct {
    float timestamp_ms;             /* When pulse started */
    float duration_ms;              /* Pulse width */
    float accumulated_energy;       /* Energy accumulated during pulse */
    float baseline_energy;          /* Baseline at detection */
    float snr_db;                   /* Signal-to-noise ratio */
} bcd_freq_event_t;

typedef void (*bcd_freq_callback_fn)(const bcd_freq_event_t *event, void *user_data);

/*============================================================================
 * Public API
 *============================================================================*/

/**
 * Create a new BCD frequency-domain detector instance
 * @param csv_path  Path for CSV log file (NULL to disable logging)
 * @return          Detector instance or NULL on failure
 */
bcd_freq_detector_t *bcd_freq_detector_create(const char *csv_path);

/**
 * Destroy a BCD frequency-domain detector instance
 */
void bcd_freq_detector_destroy(bcd_freq_detector_t *fd);

/**
 * Set callback for pulse events
 * @param fd        Detector instance
 * @param callback  Function to call when pulse detected
 * @param user_data Passed to callback
 */
void bcd_freq_detector_set_callback(bcd_freq_detector_t *fd,
                                    bcd_freq_callback_fn callback,
                                    void *user_data);

/**
 * Feed I/Q samples to detector
 * Detector buffers internally and runs FFT when ready
 * @param fd        Detector instance
 * @param i_sample  In-phase sample
 * @param q_sample  Quadrature sample
 * @return          true if a pulse was detected this sample
 */
bool bcd_freq_detector_process_sample(bcd_freq_detector_t *fd,
                                      float i_sample,
                                      float q_sample);

/**
 * Enable/disable detection
 */
void bcd_freq_detector_set_enabled(bcd_freq_detector_t *fd, bool enabled);
bool bcd_freq_detector_get_enabled(bcd_freq_detector_t *fd);

/**
 * Get current state for display/debug
 */
float bcd_freq_detector_get_accumulated_energy(bcd_freq_detector_t *fd);
float bcd_freq_detector_get_baseline(bcd_freq_detector_t *fd);
float bcd_freq_detector_get_threshold(bcd_freq_detector_t *fd);
float bcd_freq_detector_get_current_energy(bcd_freq_detector_t *fd);
int bcd_freq_detector_get_pulse_count(bcd_freq_detector_t *fd);

/**
 * Print statistics to stdout
 */
void bcd_freq_detector_print_stats(bcd_freq_detector_t *fd);

/**
 * Get timing info
 */
float bcd_freq_detector_get_frame_duration_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* BCD_FREQ_DETECTOR_H */
