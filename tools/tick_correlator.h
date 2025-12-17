/**
 * @file tick_correlator.h
 * @brief WWV tick correlation database
 *
 * Groups consecutive ticks into correlation chains when they fall
 * within 1050ms of each other. Tracks chain statistics and outputs
 * correlated tick data to CSV.
 */

#ifndef TICK_CORRELATOR_H
#define TICK_CORRELATOR_H

#include <stdbool.h>
#include <stdint.h>

typedef struct tick_correlator tick_correlator_t;

/*============================================================================
 * Configuration
 *============================================================================*/

#define CORR_MAX_INTERVAL_MS    1050.0f   /* Max interval to correlate ticks */
#define CORR_MIN_INTERVAL_MS    900.0f    /* Min expected interval */
#define CORR_NOMINAL_INTERVAL   1000.0f   /* Expected tick interval */

/*============================================================================
 * Tick Record (matches tick_detector CSV output + correlation fields)
 *============================================================================*/

typedef struct {
    /* From tick_detector */
    char time_str[16];          /* Wall clock HH:MM:SS */
    float timestamp_ms;         /* ms since start */
    int tick_num;               /* Tick number from detector */
    char expected[16];          /* WWV expected event */
    float energy_peak;          /* Peak energy */
    float duration_ms;          /* Pulse duration */
    float interval_ms;          /* Interval from previous tick */
    float avg_interval_ms;      /* Running average interval */
    float noise_floor;          /* Energy noise floor */
    float corr_peak;            /* Correlation peak */
    float corr_ratio;           /* Correlation ratio */
    
    /* Correlation fields */
    int chain_id;               /* Correlation chain ID (0 = uncorrelated) */
    int chain_position;         /* Position within chain (1, 2, 3...) */
    float chain_start_ms;       /* Timestamp of chain start */
    float drift_ms;             /* Cumulative drift from nominal */
} tick_record_t;

/*============================================================================
 * Chain Statistics
 *============================================================================*/

typedef struct {
    int chain_id;
    int tick_count;
    /* Count of ticks inferred due to single-tick dropouts (1900-2100ms interval).
     * On HF, brief fades or QRN bursts can cause one tick to be missed. Rather
     * than breaking the chain, we allow single-skip intervals and track how many
     * were inferred. Higher inferred_count = lower chain quality. Added v1.0.1+19. */
    int inferred_count;
    float start_ms;
    float end_ms;
    float total_drift_ms;       /* Accumulated drift from nominal */
    float avg_interval_ms;
    float min_interval_ms;
    float max_interval_ms;
} chain_stats_t;

/*============================================================================
 * API
 *============================================================================*/

/* Create/destroy */
tick_correlator_t *tick_correlator_create(const char *csv_path);
void tick_correlator_destroy(tick_correlator_t *tc);

/* Add tick from detector (call for each tick event) */
void tick_correlator_add_tick(tick_correlator_t *tc,
                              const char *time_str,
                              float timestamp_ms,
                              int tick_num,
                              const char *expected,
                              float energy_peak,
                              float duration_ms,
                              float interval_ms,
                              float avg_interval_ms,
                              float noise_floor,
                              float corr_peak,
                              float corr_ratio);

/* Query */
int tick_correlator_get_chain_count(tick_correlator_t *tc);
int tick_correlator_get_current_chain_length(tick_correlator_t *tc);
float tick_correlator_get_current_drift(tick_correlator_t *tc);
chain_stats_t tick_correlator_get_chain_stats(tick_correlator_t *tc, int chain_id);

/* Print summary */
void tick_correlator_print_stats(tick_correlator_t *tc);

#endif /* TICK_CORRELATOR_H */
