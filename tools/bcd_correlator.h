/**
 * @file bcd_correlator.h
 * @brief WWV BCD correlator - combines time and freq detector outputs
 *
 * Receives events from both bcd_time_detector (precise timing) and
 * bcd_freq_detector (confident 100Hz identification) and correlates them
 * to produce high-confidence BCD symbols.
 *
 * Pattern: Follows sync_detector.h structure
 *
 * State progression:
 *   ACQUIRING -> TENTATIVE -> TRACKING
 *
 * A confirmed symbol requires both detectors to fire within a correlation
 * window with matching pulse durations.
 */

#ifndef BCD_CORRELATOR_H
#define BCD_CORRELATOR_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Configuration
 *============================================================================*/

/* Correlation parameters */
#define BCD_CORR_WINDOW_MS          100.0f  /* Detections within 100ms */
#define BCD_CORR_DURATION_TOL       0.25f   /* 25% duration agreement tolerance */
#define BCD_CORR_LOCKOUT_MS         200.0f  /* Prevent duplicate symbols */
#define BCD_CORR_MIN_INTERVAL_MS    800.0f  /* Min time between symbols */
#define BCD_CORR_PENDING_TIMEOUT_MS 500.0f  /* Clear pending if no partner */

/* Symbol width thresholds (milliseconds) - same as bcd_decoder.h */
#define BCD_SYMBOL_ZERO_MAX_MS      350.0f  /* 100-350ms = binary 0 */
#define BCD_SYMBOL_ONE_MAX_MS       650.0f  /* 350-650ms = binary 1 */
#define BCD_SYMBOL_MARKER_MAX_MS    900.0f  /* 650-900ms = position marker */

/*============================================================================
 * Types
 *============================================================================*/

/* Symbol types - matches bcd_decoder.h */
typedef enum {
    BCD_CORR_SYM_NONE = -1,
    BCD_CORR_SYM_ZERO = 0,
    BCD_CORR_SYM_ONE = 1,
    BCD_CORR_SYM_MARKER = 2
} bcd_corr_symbol_t;

/* Correlator confidence states */
typedef enum {
    BCD_CORR_ACQUIRING,     /* No confirmed symbols yet */
    BCD_CORR_TENTATIVE,     /* 1-2 confirmed symbols */
    BCD_CORR_TRACKING       /* 3+ symbols with ~1s intervals */
} bcd_corr_state_t;

/* Symbol event for callback */
typedef struct {
    bcd_corr_symbol_t symbol;
    float timestamp_ms;
    float duration_ms;
    float confidence;           /* 0-1, higher if both detectors agree */
    const char *source;         /* "BOTH", "TIME", or "FREQ" */
} bcd_symbol_event_t;

typedef void (*bcd_corr_symbol_callback_fn)(const bcd_symbol_event_t *event, void *user_data);

/* Opaque handle */
typedef struct bcd_correlator bcd_correlator_t;

/*============================================================================
 * API
 *============================================================================*/

/**
 * Create BCD correlator
 * @param csv_path Path for CSV log file (NULL to disable logging)
 * @return Correlator handle or NULL on failure
 */
bcd_correlator_t *bcd_correlator_create(const char *csv_path);

/**
 * Destroy correlator and free resources
 */
void bcd_correlator_destroy(bcd_correlator_t *corr);

/**
 * Set callback for confirmed symbol events
 */
void bcd_correlator_set_callback(bcd_correlator_t *corr,
                                 bcd_corr_symbol_callback_fn callback,
                                 void *user_data);

/**
 * Report pulse from time detector
 * @param corr       Correlator handle
 * @param timestamp_ms Timestamp when pulse started
 * @param duration_ms  Pulse duration
 * @param peak_energy  Peak energy during pulse
 */
void bcd_correlator_time_event(bcd_correlator_t *corr,
                               float timestamp_ms,
                               float duration_ms,
                               float peak_energy);

/**
 * Report pulse from freq detector
 * @param corr            Correlator handle
 * @param timestamp_ms    Timestamp when pulse started
 * @param duration_ms     Pulse duration
 * @param accum_energy    Accumulated energy
 */
void bcd_correlator_freq_event(bcd_correlator_t *corr,
                               float timestamp_ms,
                               float duration_ms,
                               float accum_energy);

/**
 * Get current state
 */
bcd_corr_state_t bcd_correlator_get_state(bcd_correlator_t *corr);

/**
 * Get state as string for display
 */
const char *bcd_corr_state_name(bcd_corr_state_t state);

/**
 * Get symbol type as character
 */
char bcd_corr_symbol_char(bcd_corr_symbol_t sym);

/**
 * Get timestamp of last confirmed symbol
 */
float bcd_correlator_get_last_symbol_ms(bcd_correlator_t *corr);

/**
 * Get count of confirmed symbols
 */
int bcd_correlator_get_symbol_count(bcd_correlator_t *corr);

/**
 * Print statistics to stdout
 */
void bcd_correlator_print_stats(bcd_correlator_t *corr);

#ifdef __cplusplus
}
#endif

#endif /* BCD_CORRELATOR_H */
