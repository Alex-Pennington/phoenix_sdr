/**
 * @file bcd_envelope.h
 * @brief WWV 100 Hz BCD envelope tracker
 *
 * @deprecated This module is DEPRECATED. Use bcd_time_detector + bcd_freq_detector
 *             + bcd_correlator for robust dual-path BCD symbol detection.
 *             See BCD_Robust_Symbol_Demodulator_Roadmad.md for details.
 *
 * Tracks the amplitude envelope of the 100 Hz tone used for BCD time code.
 * Uses Goertzel algorithm for efficient single-frequency detection with
 * explicit DC rejection.
 *
 * Unlike tick_detector which detects pulse ON/OFF transitions, this tracker
 * measures continuous SNR of the 100 Hz tone and outputs envelope for later
 * BCD pulse width decoding by bcd_decoder.
 *
 * Signal characteristics:
 *   - 100 Hz tone is ALWAYS present (continuous)
 *   - Amplitude modulated with BCD data (200/500/800ms pulses)
 *   - 1 pulse per second rate
 *   - Must reject DC and low-frequency noise
 */

#ifndef BCD_ENVELOPE_H
#define BCD_ENVELOPE_H

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
#define BCD_ENV_SAMPLE_RATE      2400    /* Final sample rate */
#define BCD_ENV_DECIMATION       5       /* From 12 kHz display path */

/* Goertzel parameters */
#define BCD_ENV_TARGET_FREQ_HZ   100     /* BCD subcarrier frequency */
#define BCD_ENV_BLOCK_SIZE       24      /* 10 ms at 2400 Hz */
#define BCD_ENV_BLOCKS_PER_SEC   100     /* Envelope resolution */

/* DC blocking */
#define BCD_ENV_DC_ALPHA         0.995f  /* DC blocker coefficient */

/* Noise estimation */
#define BCD_ENV_NOISE_BINS       5       /* Adjacent bins for noise floor */
#define BCD_ENV_NOISE_PERCENTILE 10      /* Lower percentile for floor */

/* Envelope smoothing */
#define BCD_ENV_ALPHA            0.3f    /* Exponential smoothing */

/* SNR thresholds */
#define BCD_ENV_MIN_SNR_DB       6.0f    /* Minimum for "present" */
#define BCD_ENV_GOOD_SNR_DB      12.0f   /* Good signal quality */

/*============================================================================
 * Detector State (opaque to caller)
 *============================================================================*/

typedef struct bcd_envelope bcd_envelope_t;

/*============================================================================
 * Status/Quality Indication
 *============================================================================*/

typedef enum {
    BCD_ENV_ABSENT = 0,         /* No 100 Hz detected */
    BCD_ENV_WEAK,               /* Present but SNR < 6 dB */
    BCD_ENV_PRESENT,            /* SNR 6-12 dB */
    BCD_ENV_STRONG              /* SNR > 12 dB */
} bcd_envelope_status_t;

/*============================================================================
 * Callback for envelope updates (for BCD decoder)
 *============================================================================*/

typedef struct {
    float timestamp_ms;         /* Time since start */
    float envelope;             /* Smoothed 100 Hz magnitude (linear) */
    float envelope_db;          /* Magnitude in dB */
    float noise_floor_db;       /* Current noise estimate */
    float snr_db;               /* Signal-to-noise ratio */
    bcd_envelope_status_t status; /* Quality indication */
} bcd_envelope_frame_t;

typedef void (*bcd_envelope_callback_fn)(const bcd_envelope_frame_t *frame, void *user_data);

/*============================================================================
 * Public API
 *============================================================================*/

/**
 * Create a new BCD envelope tracker instance
 * @param csv_path  Path for CSV log file (NULL to disable logging)
 * @return          Tracker instance or NULL on failure
 */
bcd_envelope_t *bcd_envelope_create(const char *csv_path);

/**
 * Destroy a BCD envelope tracker instance
 */
void bcd_envelope_destroy(bcd_envelope_t *det);

/**
 * Set callback for envelope frames (100 per second)
 * @param det       Tracker instance
 * @param callback  Function to call every 10 ms
 * @param user_data Passed to callback
 */
void bcd_envelope_set_callback(bcd_envelope_t *det,
                               bcd_envelope_callback_fn callback,
                               void *user_data);

/**
 * Feed samples from 12 kHz display path
 * Tracker decimates to 2.4 kHz internally
 * @param det       Tracker instance
 * @param i_sample  In-phase sample (12 kHz rate)
 * @param q_sample  Quadrature sample (12 kHz rate)
 */
void bcd_envelope_process_sample(bcd_envelope_t *det,
                                 float i_sample, float q_sample);

/**
 * Alternative: Feed pre-decimated samples at 2.4 kHz
 * Use this if decimation happens elsewhere
 */
void bcd_envelope_process_sample_2400(bcd_envelope_t *det,
                                      float i_sample, float q_sample);

/**
 * Enable/disable tracking
 */
void bcd_envelope_set_enabled(bcd_envelope_t *det, bool enabled);
bool bcd_envelope_get_enabled(bcd_envelope_t *det);

/*============================================================================
 * Status Getters (for UI display)
 *============================================================================*/

/**
 * Get current SNR in dB
 */
float bcd_envelope_get_snr_db(bcd_envelope_t *det);

/**
 * Get current envelope (linear magnitude)
 */
float bcd_envelope_get_envelope(bcd_envelope_t *det);

/**
 * Get current status
 */
bcd_envelope_status_t bcd_envelope_get_status(bcd_envelope_t *det);

/**
 * Get noise floor estimate in dB
 */
float bcd_envelope_get_noise_floor_db(bcd_envelope_t *det);

/**
 * Get positive/negative sideband magnitudes (for symmetry check)
 * Returns true if both sidebands are similar (good AM signal)
 */
bool bcd_envelope_get_sideband_balance(bcd_envelope_t *det,
                                       float *pos_mag, float *neg_mag);

/**
 * Print statistics to stdout
 */
void bcd_envelope_print_stats(bcd_envelope_t *det);

#ifdef __cplusplus
}
#endif

#endif /* BCD_ENVELOPE_H */
