/**
 * @file bcd_time_detector.h
 * @brief WWV BCD time-domain detector module
 *
 * Time-domain focused FFT for precise pulse edge detection at 100Hz.
 * Uses small FFT (256) for fast time response at expense of frequency resolution.
 *
 * Pattern: Follows tick_detector.h structure
 *
 * Design notes:
 *   - Self-contained detector with own FFT (50kHz, 256-pt)
 *   - Own sample buffer and state machine
 *   - Designed to run in parallel with bcd_freq_detector
 *   - Reports events with precise edge timestamps
 *   - Works with bcd_correlator for dual-path symbol confirmation
 */

#ifndef BCD_TIME_DETECTOR_H
#define BCD_TIME_DETECTOR_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Configuration - Tune these for 100Hz BCD signal
 *============================================================================*/

#define BCD_TIME_FFT_SIZE           256     /* 5.12ms frames at 50kHz */
#define BCD_TIME_SAMPLE_RATE        50000   /* Input sample rate (2MHz/40) */
#define BCD_TIME_TARGET_FREQ_HZ     100     /* BCD subcarrier frequency */
#define BCD_TIME_BANDWIDTH_HZ       50      /* Wider bucket for coarse resolution */

/* Detection parameters */
#define BCD_TIME_THRESHOLD_MULT     2.0f    /* Energy must be 2x noise floor */
#define BCD_TIME_HYSTERESIS_RATIO   0.7f    /* Drop-off threshold */

/* Pulse classification thresholds (same as existing bcd_decoder) */
#define BCD_TIME_PULSE_MIN_MS       100.0f  /* Ignore pulses shorter than this */
#define BCD_TIME_PULSE_MAX_MS       1000.0f /* Ignore pulses longer than this */

/*============================================================================
 * Detector State (opaque to caller)
 *============================================================================*/

typedef struct bcd_time_detector bcd_time_detector_t;

/*============================================================================
 * Callback for pulse events
 *============================================================================*/

typedef struct {
    float timestamp_ms;         /* When pulse started */
    float duration_ms;          /* Pulse width */
    float peak_energy;          /* Peak energy during pulse */
    float noise_floor;          /* Noise floor at detection */
    float snr_db;               /* Signal-to-noise ratio */
} bcd_time_event_t;

typedef void (*bcd_time_callback_fn)(const bcd_time_event_t *event, void *user_data);

/*============================================================================
 * Public API
 *============================================================================*/

/**
 * Create a new BCD time-domain detector instance
 * @param csv_path  Path for CSV log file (NULL to disable logging)
 * @return          Detector instance or NULL on failure
 */
bcd_time_detector_t *bcd_time_detector_create(const char *csv_path);

/**
 * Destroy a BCD time-domain detector instance
 */
void bcd_time_detector_destroy(bcd_time_detector_t *td);

/**
 * Set callback for pulse events
 * @param td        Detector instance
 * @param callback  Function to call when pulse detected
 * @param user_data Passed to callback
 */
void bcd_time_detector_set_callback(bcd_time_detector_t *td,
                                    bcd_time_callback_fn callback,
                                    void *user_data);

/**
 * Feed I/Q samples to detector
 * Detector buffers internally and runs FFT when ready
 * @param td        Detector instance
 * @param i_sample  In-phase sample
 * @param q_sample  Quadrature sample
 * @return          true if a pulse was detected this sample
 */
bool bcd_time_detector_process_sample(bcd_time_detector_t *td,
                                      float i_sample,
                                      float q_sample);

/**
 * Enable/disable detection
 */
void bcd_time_detector_set_enabled(bcd_time_detector_t *td, bool enabled);
bool bcd_time_detector_get_enabled(bcd_time_detector_t *td);

/**
 * Get current state for display/debug
 */
float bcd_time_detector_get_noise_floor(bcd_time_detector_t *td);
float bcd_time_detector_get_threshold(bcd_time_detector_t *td);
float bcd_time_detector_get_current_energy(bcd_time_detector_t *td);
int bcd_time_detector_get_pulse_count(bcd_time_detector_t *td);

/**
 * Print statistics to stdout
 */
void bcd_time_detector_print_stats(bcd_time_detector_t *td);

/**
 * Get timing info
 */
float bcd_time_detector_get_frame_duration_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* BCD_TIME_DETECTOR_H */
