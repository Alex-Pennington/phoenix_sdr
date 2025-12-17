/**
 * @file subcarrier_detector.h
 * @brief WWV 100 Hz BCD subcarrier detector
 *
 * Detects presence of the continuous 100 Hz subcarrier used for BCD time code.
 * Uses Goertzel algorithm for efficient single-frequency detection with
 * explicit DC rejection.
 *
 * Unlike tick_detector which detects pulse ON/OFF transitions, this detector
 * tracks continuous SNR of the 100 Hz tone and outputs envelope for later
 * BCD pulse width decoding.
 *
 * Signal characteristics:
 *   - 100 Hz tone is ALWAYS present (continuous)
 *   - Amplitude modulated with BCD data (200/500/800ms pulses)
 *   - 1 pulse per second rate
 *   - Must reject DC and low-frequency noise
 */

#ifndef SUBCARRIER_DETECTOR_H
#define SUBCARRIER_DETECTOR_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Configuration
 *============================================================================*/

/* Decimation path: 2 MHz -> 12 kHz -> 2.4 kHz (divide by 5) */
#define SUBCARRIER_SAMPLE_RATE      2400    /* Final sample rate */
#define SUBCARRIER_DECIMATION       5       /* From 12 kHz display path */

/* Goertzel parameters */
#define SUBCARRIER_TARGET_FREQ_HZ   100     /* BCD subcarrier frequency */
#define SUBCARRIER_BLOCK_SIZE       24      /* 10 ms at 2400 Hz */
#define SUBCARRIER_BLOCKS_PER_SEC   100     /* Envelope resolution */

/* DC blocking */
#define SUBCARRIER_DC_ALPHA         0.995f  /* DC blocker coefficient */

/* Noise estimation */
#define SUBCARRIER_NOISE_BINS       5       /* Adjacent bins for noise floor */
#define SUBCARRIER_NOISE_PERCENTILE 10      /* Lower percentile for floor */

/* Envelope smoothing */
#define SUBCARRIER_ENV_ALPHA        0.3f    /* Exponential smoothing */

/* SNR thresholds */
#define SUBCARRIER_MIN_SNR_DB       6.0f    /* Minimum for "present" */
#define SUBCARRIER_GOOD_SNR_DB      12.0f   /* Good signal quality */

/*============================================================================
 * Detector State (opaque to caller)
 *============================================================================*/

typedef struct subcarrier_detector subcarrier_detector_t;

/*============================================================================
 * Status/Quality Indication
 *============================================================================*/

typedef enum {
    SUBCARRIER_ABSENT = 0,      /* No 100 Hz detected */
    SUBCARRIER_WEAK,            /* Present but SNR < 6 dB */
    SUBCARRIER_PRESENT,         /* SNR 6-12 dB */
    SUBCARRIER_STRONG           /* SNR > 12 dB */
} subcarrier_status_t;

/*============================================================================
 * Callback for envelope updates (for BCD decoder)
 *============================================================================*/

typedef struct {
    float timestamp_ms;         /* Time since start */
    float envelope;             /* Smoothed 100 Hz magnitude (linear) */
    float envelope_db;          /* Magnitude in dB */
    float noise_floor_db;       /* Current noise estimate */
    float snr_db;               /* Signal-to-noise ratio */
    subcarrier_status_t status; /* Quality indication */
} subcarrier_frame_t;

typedef void (*subcarrier_callback_fn)(const subcarrier_frame_t *frame, void *user_data);

/*============================================================================
 * Public API
 *============================================================================*/

/**
 * Create a new subcarrier detector instance
 * @param csv_path  Path for CSV log file (NULL to disable logging)
 * @return          Detector instance or NULL on failure
 */
subcarrier_detector_t *subcarrier_detector_create(const char *csv_path);

/**
 * Destroy a subcarrier detector instance
 */
void subcarrier_detector_destroy(subcarrier_detector_t *det);

/**
 * Set callback for envelope frames (100 per second)
 * @param det       Detector instance
 * @param callback  Function to call every 10 ms
 * @param user_data Passed to callback
 */
void subcarrier_detector_set_callback(subcarrier_detector_t *det,
                                      subcarrier_callback_fn callback,
                                      void *user_data);

/**
 * Feed samples from 12 kHz display path
 * Detector decimates to 2.4 kHz internally
 * @param det       Detector instance
 * @param i_sample  In-phase sample (12 kHz rate)
 * @param q_sample  Quadrature sample (12 kHz rate)
 */
void subcarrier_detector_process_sample(subcarrier_detector_t *det,
                                        float i_sample, float q_sample);

/**
 * Alternative: Feed pre-decimated samples at 2.4 kHz
 * Use this if decimation happens elsewhere
 */
void subcarrier_detector_process_sample_2400(subcarrier_detector_t *det,
                                             float i_sample, float q_sample);

/**
 * Enable/disable detection
 */
void subcarrier_detector_set_enabled(subcarrier_detector_t *det, bool enabled);
bool subcarrier_detector_get_enabled(subcarrier_detector_t *det);

/*============================================================================
 * Status Getters (for UI display)
 *============================================================================*/

/**
 * Get current SNR in dB
 */
float subcarrier_detector_get_snr_db(subcarrier_detector_t *det);

/**
 * Get current envelope (linear magnitude)
 */
float subcarrier_detector_get_envelope(subcarrier_detector_t *det);

/**
 * Get current status
 */
subcarrier_status_t subcarrier_detector_get_status(subcarrier_detector_t *det);

/**
 * Get noise floor estimate in dB
 */
float subcarrier_detector_get_noise_floor_db(subcarrier_detector_t *det);

/**
 * Get positive/negative sideband magnitudes (for symmetry check)
 * Returns true if both sidebands are similar (good AM signal)
 */
bool subcarrier_detector_get_sideband_balance(subcarrier_detector_t *det,
                                              float *pos_mag, float *neg_mag);

/**
 * Print statistics to stdout
 */
void subcarrier_detector_print_stats(subcarrier_detector_t *det);

#ifdef __cplusplus
}
#endif

#endif /* SUBCARRIER_DETECTOR_H */
