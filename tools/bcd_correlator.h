/**
 * @file bcd_correlator.h
 * @brief WWV BCD Window-Based Symbol Demodulator
 *
 * ARCHITECTURE (v2 - Window-Based):
 *   - Gates on sync_detector LOCKED state
 *   - Uses minute anchor to define 1-second windows
 *   - Integrates energy from time/freq detectors over each window
 *   - Classifies ONCE per window at window close
 *   - Emits exactly one symbol per second (when sync locked)
 *
 * State progression:
 *   ACQUIRING -> TENTATIVE -> TRACKING
 *
 * Unlike event-correlation approach, this guarantees no duplicate symbols
 * because classification happens once per window, not per event.
 */

#ifndef BCD_CORRELATOR_H
#define BCD_CORRELATOR_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/* Forward declaration */
typedef struct sync_detector sync_detector_t;

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Configuration
 *============================================================================*/

/* Symbol width thresholds (milliseconds) */
#define BCD_SYMBOL_ZERO_MAX_MS      350.0f  /* 100-350ms = binary 0 */
#define BCD_SYMBOL_ONE_MAX_MS       650.0f  /* 350-650ms = binary 1 */
#define BCD_SYMBOL_MARKER_MAX_MS    900.0f  /* 650-900ms = position marker */

/*============================================================================
 * Types
 *============================================================================*/

/* Symbol types */
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
    float confidence;           /* 0-1, higher if both detectors contributed */
    const char *source;         /* "BOTH", "TIME", "FREQ", or "NONE" */
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
 * Link sync detector as timing reference
 * REQUIRED: Correlator will not emit symbols until sync is LOCKED
 * @param corr   Correlator handle
 * @param sync   Sync detector to use as timing reference
 */
void bcd_correlator_set_sync_source(bcd_correlator_t *corr, sync_detector_t *sync);

/**
 * Set callback for confirmed symbol events
 */
void bcd_correlator_set_callback(bcd_correlator_t *corr,
                                 bcd_corr_symbol_callback_fn callback,
                                 void *user_data);

/**
 * Report pulse from time detector
 * Event is accumulated into current 1-second window
 * @param corr         Correlator handle
 * @param timestamp_ms Timestamp when pulse detected
 * @param duration_ms  Pulse duration
 * @param peak_energy  Peak energy during pulse
 */
void bcd_correlator_time_event(bcd_correlator_t *corr,
                               float timestamp_ms,
                               float duration_ms,
                               float peak_energy);

/**
 * Report pulse from freq detector
 * Event is accumulated into current 1-second window
 * @param corr            Correlator handle
 * @param timestamp_ms    Timestamp when pulse detected
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
 * Get symbol type as character ('0', '1', 'P', or '.' for none)
 */
char bcd_corr_symbol_char(bcd_corr_symbol_t sym);

/**
 * Get timestamp of last emitted symbol
 */
float bcd_correlator_get_last_symbol_ms(bcd_correlator_t *corr);

/**
 * Get count of emitted symbols
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
