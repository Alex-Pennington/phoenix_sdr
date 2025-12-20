/**
 * @file tick_correlator.c
 * @brief WWV tick correlation database implementation
 *
 * Groups consecutive ticks into correlation chains. A chain continues
 * as long as ticks arrive within 1050ms of each other. Tracks drift
 * from nominal 1000ms interval.
 */

#include "tick_correlator.h"
#include "waterfall_telemetry.h"
#include "version.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

/*============================================================================
 * Configuration
 *============================================================================*/

#define MAX_CHAINS          1000    /* Max chains to track stats for */
#define MAX_TICKS_STORED    10000   /* Max ticks to keep in memory */

/*============================================================================
 * Internal State
 *============================================================================*/

struct tick_correlator {
    /* Tick storage */
    tick_record_t *ticks;
    int tick_count;
    int tick_capacity;

    /* Chain tracking */
    chain_stats_t *chains;
    int chain_count;
    int chain_capacity;

    /* Current chain state */
    int current_chain_id;
    int current_chain_length;
    float current_chain_start_ms;
    float last_tick_ms;
    float cumulative_drift_ms;

    /* Overall stats */
    int total_correlated;
    int total_uncorrelated;
    float longest_chain_ticks;

    /* Logging */
    FILE *csv_file;
    time_t start_time;

    /* Epoch callback */
    epoch_callback_fn epoch_callback;
    void *epoch_callback_user_data;

    /* Interval tracking for std_dev calculation */
    float recent_intervals[5];
    int recent_interval_idx;
    int recent_interval_count;

    /* Prediction-based tracking state */
    struct {
        bool active;                    /* Tracking loop engaged */
        int retained_chain_id;          /* Chain to reattach to */
        float predicted_next_ms;        /* When we expect next tick */
        float discipline_window_ms;     /* Acceptance window (4σ) */
        float last_std_dev_ms;          /* For discipline monitoring */
        int consecutive_misses;         /* Count prediction failures */
    } tracking;

    /* Tunable parameters (runtime adjustable via UDP commands) */
    float epoch_confidence_threshold;   /* Min confidence to activate tracking (0.5-0.95, default 0.8) */
    int max_consecutive_misses;         /* Misses before dropping lock (2-10, default 5) */
};

/*============================================================================
 * Internal Functions
 *============================================================================*/

static void start_new_chain(tick_correlator_t *tc, float timestamp_ms) {
    tc->chain_count++;
    tc->current_chain_id = tc->chain_count;
    tc->current_chain_length = 0;
    tc->current_chain_start_ms = timestamp_ms;
    tc->cumulative_drift_ms = 0.0f;

    /* Reset interval tracking for epoch calculation */
    tc->recent_interval_idx = 0;
    tc->recent_interval_count = 0;
    memset(tc->recent_intervals, 0, sizeof(tc->recent_intervals));

    /* Initialize chain stats */
    if (tc->chain_count <= tc->chain_capacity) {
        chain_stats_t *cs = &tc->chains[tc->chain_count - 1];
        cs->chain_id = tc->current_chain_id;
        cs->tick_count = 0;
        cs->inferred_count = 0;
        cs->start_ms = timestamp_ms;
        cs->end_ms = timestamp_ms;
        cs->total_drift_ms = 0.0f;
        cs->avg_interval_ms = 0.0f;
        cs->min_interval_ms = 99999.0f;
        cs->max_interval_ms = 0.0f;
    }
}

static void update_chain_stats(tick_correlator_t *tc, float interval_ms, float timestamp_ms) {
    if (tc->current_chain_id <= 0 || tc->current_chain_id > tc->chain_capacity) return;

    chain_stats_t *cs = &tc->chains[tc->current_chain_id - 1];
    cs->tick_count = tc->current_chain_length;
    cs->end_ms = timestamp_ms;
    cs->total_drift_ms = tc->cumulative_drift_ms;

    /* Update interval stats */
    if (interval_ms > 0) {
        if (interval_ms < cs->min_interval_ms) cs->min_interval_ms = interval_ms;
        if (interval_ms > cs->max_interval_ms) cs->max_interval_ms = interval_ms;

        /* Running average */
        float n = (float)cs->tick_count;
        cs->avg_interval_ms = ((n - 1.0f) * cs->avg_interval_ms + interval_ms) / n;
    }
}

/*============================================================================
 * Public API
 *============================================================================*/

tick_correlator_t *tick_correlator_create(const char *csv_path) {
    tick_correlator_t *tc = (tick_correlator_t *)calloc(1, sizeof(tick_correlator_t));
    if (!tc) return NULL;

    /* Allocate tick storage */
    tc->tick_capacity = MAX_TICKS_STORED;
    tc->ticks = (tick_record_t *)calloc(tc->tick_capacity, sizeof(tick_record_t));

    /* Allocate chain storage */
    tc->chain_capacity = MAX_CHAINS;
    tc->chains = (chain_stats_t *)calloc(tc->chain_capacity, sizeof(chain_stats_t));

    if (!tc->ticks || !tc->chains) {
        tick_correlator_destroy(tc);
        return NULL;
    }

    tc->start_time = time(NULL);
    tc->last_tick_ms = -99999.0f;  /* Force new chain on first tick */

    /* Initialize epoch callback */
    tc->epoch_callback = NULL;
    tc->epoch_callback_user_data = NULL;
    memset(tc->recent_intervals, 0, sizeof(tc->recent_intervals));
    tc->recent_interval_idx = 0;
    tc->recent_interval_count = 0;

    /* Initialize prediction tracking state */
    tc->tracking.active = false;
    tc->tracking.retained_chain_id = 0;
    tc->tracking.predicted_next_ms = 0.0f;
    tc->tracking.discipline_window_ms = 0.0f;
    tc->tracking.last_std_dev_ms = 0.0f;
    tc->tracking.consecutive_misses = 0;

    /* Initialize tunable parameters to defaults */
    tc->epoch_confidence_threshold = 0.8f;
    tc->max_consecutive_misses = 5;

    /* Open CSV file */
    if (csv_path) {
        tc->csv_file = fopen(csv_path, "w");
        if (tc->csv_file) {
            char time_str[64];
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S",
                     localtime(&tc->start_time));

            fprintf(tc->csv_file, "# Phoenix SDR WWV Tick Correlation Database v%s\n",
                    PHOENIX_VERSION_FULL);
            fprintf(tc->csv_file, "# Started: %s\n", time_str);
            fprintf(tc->csv_file, "# Correlation window: %.0f-%.0f ms\n",
                    CORR_MIN_INTERVAL_MS, CORR_MAX_INTERVAL_MS);
            fprintf(tc->csv_file, "time,timestamp_ms,tick_num,expected,energy_peak,duration_ms,"
                    "interval_ms,avg_interval_ms,noise_floor,corr_peak,corr_ratio,"
                    "chain_id,chain_pos,chain_start_ms,drift_ms\n");
            fflush(tc->csv_file);
        }
    }

    printf("[CORR] Tick correlator created (window: %.0f-%.0f ms)\n",
           CORR_MIN_INTERVAL_MS, CORR_MAX_INTERVAL_MS);

    return tc;
}

void tick_correlator_destroy(tick_correlator_t *tc) {
    if (!tc) return;

    if (tc->csv_file) fclose(tc->csv_file);
    free(tc->ticks);
    free(tc->chains);
    free(tc);
}

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
                              float corr_ratio) {
    if (!tc) return;

    /* Calculate interval from last tick */
    float actual_interval = timestamp_ms - tc->last_tick_ms;

    /* Prediction-based tracking: check if tick matches prediction from established discipline */
    bool prediction_match = false;
    if (tc->tracking.active && tc->last_tick_ms > 0) {
        float predicted_next = tc->last_tick_ms + CORR_NOMINAL_INTERVAL;
        float prediction_error = fabsf(timestamp_ms - predicted_next);

        /* Require BOTH timestamp AND interval discipline:
         * - Timestamp within discipline window (±10ms typical)
         * - Interval within proven range (998-1002ms) */
        if (prediction_error <= tc->tracking.discipline_window_ms &&
            actual_interval >= 998.0f && actual_interval <= 1002.0f) {
            /* Both timestamp and interval match discipline - accept it */
            prediction_match = true;
            tc->tracking.consecutive_misses = 0;
            telem_sendf(TELEM_CONSOLE, "[TRACK] HIT: t_err=%.1fms int=%.1fms chain=%d",
                       prediction_error, actual_interval, tc->tracking.retained_chain_id);

            /* Reattach to retained chain if we broke off */
            if (tc->current_chain_id != tc->tracking.retained_chain_id) {
                telem_sendf(TELEM_CONSOLE, "[TRACK] Reattach: %d → %d",
                           tc->current_chain_id, tc->tracking.retained_chain_id);
                tc->current_chain_id = tc->tracking.retained_chain_id;
            }
        } else {
            /* Prediction miss - increment miss counter */
            tc->tracking.consecutive_misses++;
            telem_sendf(TELEM_CONSOLE, "[TRACK] MISS: t_err=%.1fms int=%.1fms misses=%d",
                       prediction_error, actual_interval, tc->tracking.consecutive_misses);

            /* Exit tracking after max consecutive misses */
            if (tc->tracking.consecutive_misses >= tc->max_consecutive_misses) {
                telem_sendf(TELEM_CONSOLE, "[TRACK] Deactivated: %d consecutive misses, signal lost",
                           tc->max_consecutive_misses);
                tc->tracking.active = false;
                tc->tracking.consecutive_misses = 0;
            }
        }
    }

    /* Determine if this tick correlates with previous */
    bool correlates = prediction_match ||
                      (actual_interval >= CORR_MIN_INTERVAL_MS &&
                       actual_interval <= CORR_MAX_INTERVAL_MS);
    /* Grace period for single-tick dropouts: if interval is ~2 seconds (1900-2100ms),
     * assume exactly one tick was missed due to RF fade or QRN burst. Continue the
     * chain rather than breaking it. Split the drift evenly across both ticks.
     * Common on HF where brief fades are frequent. Added v1.0.1+19, 2025-12-17. */
    bool one_skip = (actual_interval >= 1900.0f && actual_interval <= 2100.0f);

    /* Calculate drift from nominal */
    float drift_this_tick = 0.0f;

    if (!correlates && !one_skip) {
        /* Start new chain - neither normal interval nor single skip */
        start_new_chain(tc, timestamp_ms);
        tc->total_uncorrelated++;
    } else if (one_skip && tc->current_chain_id != 0) {
        /* Single tick dropout - continue chain, split drift across both */
        drift_this_tick = (actual_interval - 2000.0f) / 2.0f;
        tc->total_correlated++;
        /* Increment inferred count for this chain */
        if (tc->current_chain_id > 0 && tc->current_chain_id <= tc->chain_capacity) {
            tc->chains[tc->current_chain_id - 1].inferred_count++;
        }
    } else if (tc->current_chain_id == 0) {
        /* First tick or after uncorrelated - start new chain */
        start_new_chain(tc, timestamp_ms);
        tc->total_uncorrelated++;
    } else {
        /* Normal correlation */
        drift_this_tick = actual_interval - CORR_NOMINAL_INTERVAL;
        tc->total_correlated++;
    }

    /* Add to current chain */
    tc->current_chain_length++;
    tc->cumulative_drift_ms += drift_this_tick;

    /* Update chain stats */
    update_chain_stats(tc, actual_interval, timestamp_ms);

    /* Track longest chain */
    if (tc->current_chain_length > tc->longest_chain_ticks) {
        tc->longest_chain_ticks = tc->current_chain_length;
    }

    /* Track recent intervals for epoch calculation
     * Use actual_interval (timestamp delta) not interval_ms (tick_detector's average)
     * Only track when chain_length > 1 (first tick has no interval) */
    if (actual_interval > 0 && tc->current_chain_length > 1) {
        tc->recent_intervals[tc->recent_interval_idx] = actual_interval;
        tc->recent_interval_idx = (tc->recent_interval_idx + 1) % 5;
        if (tc->recent_interval_count < 5) {
            tc->recent_interval_count++;
        }
    }

    /* Calculate epoch when we have 4+ correlated intervals (5+ ticks in chain)
     * NOTE: 5 ticks = 4 intervals, sufficient for std_dev calculation */
    if (tc->current_chain_length >= 5 && tc->recent_interval_count >= 4 && tc->epoch_callback) {
        /* Calculate std_dev from recent intervals */
        float sum = 0, sum_sq = 0;
        int n = tc->recent_interval_count;
        for (int i = 0; i < n; i++) {
            sum += tc->recent_intervals[i];
            sum_sq += tc->recent_intervals[i] * tc->recent_intervals[i];
        }
        float mean = sum / n;
        float variance = (sum_sq / n) - (mean * mean);
        float std_dev_ms = sqrtf(variance > 0 ? variance : 0);

        /* Calculate confidence (1.0 when std_dev=0, 0.0 when std_dev≥50ms) */
        float confidence = 1.0f - (std_dev_ms / 50.0f);
        if (confidence < 0) confidence = 0;
        if (confidence > 1.0f) confidence = 1.0f;

        /* Update discipline window if tracking active */
        if (tc->tracking.active) {
            tc->tracking.discipline_window_ms = std_dev_ms * 4.0f;
            tc->tracking.last_std_dev_ms = std_dev_ms;

            /* Deactivate tracking if discipline degraded beyond threshold */
            if (std_dev_ms > 20.0f) {
                telem_sendf(TELEM_CONSOLE, "[TRACK] Deactivated: discipline degraded (std_dev=%.1fms > 20ms)",
                           std_dev_ms);
                tc->tracking.active = false;
                tc->tracking.consecutive_misses = 0;
            }
        }

        /* DEBUG: Show epoch calculation */
        printf("[CORR_EPOCH] chain=%d intervals=%d mean=%.1f std_dev=%.1f conf=%.3f\n",
               tc->current_chain_length, n, mean, std_dev_ms, confidence);

        /* Only call if confidence is reasonable (std_dev < 10ms) */
        if (confidence > tc->epoch_confidence_threshold) {
            float epoch_offset_ms = fmodf(timestamp_ms, 1000.0f);
            if (epoch_offset_ms < 0) epoch_offset_ms += 1000.0f;
            tc->epoch_callback(epoch_offset_ms, std_dev_ms, confidence, tc->epoch_callback_user_data);

            /* Activate prediction-based tracking - discipline achieved */
            if (!tc->tracking.active) {
                tc->tracking.active = true;
                tc->tracking.retained_chain_id = tc->current_chain_id;
                tc->tracking.discipline_window_ms = std_dev_ms * 4.0f;  /* 4σ confidence interval */
                tc->tracking.last_std_dev_ms = std_dev_ms;
                tc->tracking.consecutive_misses = 0;
                telem_sendf(TELEM_CONSOLE, "[TRACK] Activated: chain=%d window=%.1fms (4σ=%.1fms)",
                           tc->current_chain_id, tc->tracking.discipline_window_ms, std_dev_ms);
            }
        }
    }

    /* Store tick record */
    if (tc->tick_count < tc->tick_capacity) {
        tick_record_t *tr = &tc->ticks[tc->tick_count];

        strncpy(tr->time_str, time_str, sizeof(tr->time_str) - 1);
        tr->timestamp_ms = timestamp_ms;
        tr->tick_num = tick_num;
        strncpy(tr->expected, expected, sizeof(tr->expected) - 1);
        tr->energy_peak = energy_peak;
        tr->duration_ms = duration_ms;
        tr->interval_ms = interval_ms;
        tr->avg_interval_ms = avg_interval_ms;
        tr->noise_floor = noise_floor;
        tr->corr_peak = corr_peak;
        tr->corr_ratio = corr_ratio;

        tr->chain_id = tc->current_chain_id;
        tr->chain_position = tc->current_chain_length;
        tr->chain_start_ms = tc->current_chain_start_ms;
        tr->drift_ms = tc->cumulative_drift_ms;

        tc->tick_count++;
    }

    /* CSV output and telemetry */
    if (tc->csv_file) {
        fprintf(tc->csv_file, "%s,%.1f,%d,%s,%.6f,%.1f,%.0f,%.0f,%.6f,%.2f,%.1f,"
                "%d,%d,%.1f,%.1f\n",
                time_str, timestamp_ms, tick_num, expected,
                energy_peak, duration_ms, interval_ms, avg_interval_ms,
                noise_floor, corr_peak, corr_ratio,
                tc->current_chain_id, tc->current_chain_length,
                tc->current_chain_start_ms, tc->cumulative_drift_ms);
        fflush(tc->csv_file);
    }

    /* UDP telemetry */
    telem_sendf(TELEM_CORR, "%s,%.1f,%d,%s,%.6f,%.1f,%.0f,%.0f,%.6f,%.2f,%.1f,%d,%d,%.1f,%.1f",
                time_str, timestamp_ms, tick_num, expected,
                energy_peak, duration_ms, interval_ms, avg_interval_ms,
                noise_floor, corr_peak, corr_ratio,
                tc->current_chain_id, tc->current_chain_length,
                tc->current_chain_start_ms, tc->cumulative_drift_ms);

    tc->last_tick_ms = timestamp_ms;
}

int tick_correlator_get_chain_count(tick_correlator_t *tc) {
    return tc ? tc->chain_count : 0;
}

int tick_correlator_get_current_chain_length(tick_correlator_t *tc) {
    return tc ? tc->current_chain_length : 0;
}

float tick_correlator_get_current_drift(tick_correlator_t *tc) {
    return tc ? tc->cumulative_drift_ms : 0.0f;
}

chain_stats_t tick_correlator_get_chain_stats(tick_correlator_t *tc, int chain_id) {
    chain_stats_t empty = {0};
    if (!tc || chain_id <= 0 || chain_id > tc->chain_count) return empty;
    return tc->chains[chain_id - 1];
}

void tick_correlator_print_stats(tick_correlator_t *tc) {
    if (!tc) return;

    printf("\n=== TICK CORRELATION STATS ===\n");
    printf("Total ticks: %d\n", tc->tick_count);
    printf("Chains: %d\n", tc->chain_count);
    printf("Correlated: %d  Uncorrelated: %d\n",
           tc->total_correlated, tc->total_uncorrelated);
    printf("Longest chain: %.0f ticks\n", tc->longest_chain_ticks);

    if (tc->current_chain_length > 0) {
        printf("Current chain: #%d, %d ticks, drift=%.1fms\n",
               tc->current_chain_id, tc->current_chain_length,
               tc->cumulative_drift_ms);
    }

    /* Show top chains */
    printf("\nTop chains by length:\n");
    int shown = 0;
    for (int i = tc->chain_count - 1; i >= 0 && shown < 5; i--) {
        chain_stats_t *cs = &tc->chains[i];
        if (cs->tick_count > 1) {
            printf("  Chain #%d: %d ticks, avg=%.1fms, drift=%.1fms\n",
                   cs->chain_id, cs->tick_count, cs->avg_interval_ms, cs->total_drift_ms);
            shown++;
        }
    }

    printf("==============================\n");
}

void tick_correlator_set_epoch_callback(tick_correlator_t *tc, epoch_callback_fn callback, void *user_data) {
    if (!tc) return;
    tc->epoch_callback = callback;
    tc->epoch_callback_user_data = user_data;
}

/*============================================================================
 * Runtime Parameter Tuning
 *============================================================================*/

void tick_correlator_set_epoch_confidence(tick_correlator_t *tc, float threshold) {
    if (!tc || threshold < 0.5f || threshold > 0.95f) return;
    tc->epoch_confidence_threshold = threshold;
}

float tick_correlator_get_epoch_confidence(tick_correlator_t *tc) {
    return tc ? tc->epoch_confidence_threshold : 0.8f;
}

void tick_correlator_set_max_misses(tick_correlator_t *tc, int max_misses) {
    if (!tc || max_misses < 2 || max_misses > 10) return;
    tc->max_consecutive_misses = max_misses;
}

int tick_correlator_get_max_misses(tick_correlator_t *tc) {
    return tc ? tc->max_consecutive_misses : 5;
}
