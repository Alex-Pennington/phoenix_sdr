/**
 * @file bcd_correlator.c
 * @brief WWV BCD correlator implementation
 *
 * Correlates inputs from bcd_time_detector and bcd_freq_detector to confirm
 * BCD pulses with high confidence. Classifies pulses into symbols (0/1/P)
 * and outputs confirmed symbols via callback.
 *
 * Pattern: Follows sync_detector.c structure
 */

#include "bcd_correlator.h"
#include "version.h"
#include "waterfall_telemetry.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

/*============================================================================
 * Internal Constants
 *============================================================================*/

#define BCD_CORR_FLASH_FRAMES   10  /* Flash duration for UI (not used currently) */

/*============================================================================
 * Internal State
 *============================================================================*/

struct bcd_correlator {
    /* Pending event from time detector */
    float pending_time_ms;
    float pending_time_duration_ms;
    float pending_time_energy;
    bool time_pending;

    /* Pending event from freq detector */
    float pending_freq_ms;
    float pending_freq_duration_ms;
    float pending_freq_energy;
    bool freq_pending;

    /* Last confirmed symbol for lockout */
    float last_symbol_ms;
    int symbol_count;
    int good_intervals;         /* Count of ~1s intervals */

    /* State */
    bcd_corr_state_t state;

    /* Callback */
    bcd_corr_symbol_callback_fn callback;
    void *callback_user_data;

    /* Logging */
    FILE *csv_file;
    time_t start_time;
};

/*============================================================================
 * Internal Functions
 *============================================================================*/

static void get_wall_time_str(bcd_correlator_t *corr, float timestamp_ms, char *buf, size_t buflen) {
    time_t event_time = corr->start_time + (time_t)(timestamp_ms / 1000.0f);
    struct tm *tm_info = localtime(&event_time);
    strftime(buf, buflen, "%H:%M:%S", tm_info);
}

/**
 * Classify pulse duration into symbol type
 */
static bcd_corr_symbol_t classify_symbol(float duration_ms) {
    if (duration_ms < 100.0f) {
        return BCD_CORR_SYM_NONE;  /* Too short */
    } else if (duration_ms <= BCD_SYMBOL_ZERO_MAX_MS) {
        return BCD_CORR_SYM_ZERO;
    } else if (duration_ms <= BCD_SYMBOL_ONE_MAX_MS) {
        return BCD_CORR_SYM_ONE;
    } else if (duration_ms <= BCD_SYMBOL_MARKER_MAX_MS) {
        return BCD_CORR_SYM_MARKER;
    }
    return BCD_CORR_SYM_NONE;  /* Too long */
}

static void confirm_symbol(bcd_correlator_t *corr, float timestamp_ms, 
                           float duration_ms, float confidence,
                           const char *source) {
    /* Lockout check - ignore if too close to last symbol */
    if (corr->last_symbol_ms > 0) {
        float since_last = timestamp_ms - corr->last_symbol_ms;
        if (since_last < BCD_CORR_MIN_INTERVAL_MS) {
            printf("[BCD_CORR] Lockout: %.0fms since last (min %.0fms)\n",
                   since_last, BCD_CORR_MIN_INTERVAL_MS);
            corr->time_pending = false;
            corr->freq_pending = false;
            return;
        }
    }

    /* Classify symbol */
    bcd_corr_symbol_t symbol = classify_symbol(duration_ms);
    if (symbol == BCD_CORR_SYM_NONE) {
        printf("[BCD_CORR] Rejected: dur=%.0fms (out of range)\n", duration_ms);
        corr->time_pending = false;
        corr->freq_pending = false;
        return;
    }

    /* Calculate interval from previous symbol */
    float interval_ms = (corr->last_symbol_ms > 0) ?
        (timestamp_ms - corr->last_symbol_ms) : 0.0f;

    /* Track good intervals (~1 second apart) */
    if (interval_ms >= 900.0f && interval_ms <= 1100.0f) {
        corr->good_intervals++;
    }

    /* Update state */
    if (corr->good_intervals >= 3) {
        corr->state = BCD_CORR_TRACKING;
    } else if (corr->symbol_count >= 1) {
        corr->state = BCD_CORR_TENTATIVE;
    }

    /* Update last symbol tracking */
    corr->last_symbol_ms = timestamp_ms;
    corr->symbol_count++;

    /* Log to CSV */
    if (corr->csv_file) {
        char time_str[16];
        get_wall_time_str(corr, timestamp_ms, time_str, sizeof(time_str));
        fprintf(corr->csv_file, "%s,%.1f,%d,%c,%s,%.0f,%.2f,%.1f,%s\n",
                time_str, timestamp_ms, corr->symbol_count,
                bcd_corr_symbol_char(symbol), source,
                duration_ms, confidence, interval_ms / 1000.0f,
                bcd_corr_state_name(corr->state));
        fflush(corr->csv_file);
    }

    /* UDP telemetry */
    telem_sendf(TELEM_BCDS, "BCDS,SYM,%c,%.1f,%.0f,%.2f,%s,%s",
                bcd_corr_symbol_char(symbol), timestamp_ms,
                duration_ms, confidence, source,
                bcd_corr_state_name(corr->state));

    /* Console output */
    printf("[BCD_CORR] Symbol #%d: '%c' at %.1fms  dur=%.0fms  conf=%.2f  src=%s  int=%.1fs  state=%s\n",
           corr->symbol_count, bcd_corr_symbol_char(symbol),
           timestamp_ms, duration_ms, confidence, source,
           interval_ms / 1000.0f, bcd_corr_state_name(corr->state));

    /* Callback */
    if (corr->callback) {
        bcd_symbol_event_t event = {
            .symbol = symbol,
            .timestamp_ms = timestamp_ms,
            .duration_ms = duration_ms,
            .confidence = confidence,
            .source = source
        };
        corr->callback(&event, corr->callback_user_data);
    }

    /* Clear both pending events */
    corr->time_pending = false;
    corr->freq_pending = false;
}

static void try_correlate(bcd_correlator_t *corr, float current_ms) {
    /* If both are pending, check if they correlate */
    if (corr->time_pending && corr->freq_pending) {
        float delta = fabsf(corr->pending_freq_ms - corr->pending_time_ms);

        if (delta < BCD_CORR_WINDOW_MS) {
            /* Check duration agreement */
            float dur_diff = fabsf(corr->pending_time_duration_ms - corr->pending_freq_duration_ms);
            float avg_dur = (corr->pending_time_duration_ms + corr->pending_freq_duration_ms) / 2.0f;
            float dur_ratio = dur_diff / avg_dur;

            if (dur_ratio <= BCD_CORR_DURATION_TOL) {
                /* Both agree! High confidence. Use time detector timestamp (more precise) */
                confirm_symbol(corr, corr->pending_time_ms, avg_dur, 1.0f, "BOTH");
                return;
            }
        }

        /* Detectors don't agree - use the earlier one with lower confidence */
        if (corr->pending_time_ms < corr->pending_freq_ms) {
            confirm_symbol(corr, corr->pending_time_ms,
                          corr->pending_time_duration_ms, 0.6f, "TIME");
        } else {
            confirm_symbol(corr, corr->pending_freq_ms,
                          corr->pending_freq_duration_ms, 0.6f, "FREQ");
        }
    }
}

static void check_timeout(bcd_correlator_t *corr, float current_ms) {
    /* If single detector fired and partner didn't arrive, confirm from single source */
    if (corr->time_pending && !corr->freq_pending) {
        if ((current_ms - corr->pending_time_ms) > BCD_CORR_PENDING_TIMEOUT_MS) {
            confirm_symbol(corr, corr->pending_time_ms,
                          corr->pending_time_duration_ms, 0.5f, "TIME");
        }
    }
    if (corr->freq_pending && !corr->time_pending) {
        if ((current_ms - corr->pending_freq_ms) > BCD_CORR_PENDING_TIMEOUT_MS) {
            confirm_symbol(corr, corr->pending_freq_ms,
                          corr->pending_freq_duration_ms, 0.5f, "FREQ");
        }
    }
}

/*============================================================================
 * Public API
 *============================================================================*/

bcd_correlator_t *bcd_correlator_create(const char *csv_path) {
    bcd_correlator_t *corr = (bcd_correlator_t *)calloc(1, sizeof(bcd_correlator_t));
    if (!corr) return NULL;

    corr->state = BCD_CORR_ACQUIRING;
    corr->start_time = time(NULL);

    if (csv_path) {
        corr->csv_file = fopen(csv_path, "w");
        if (corr->csv_file) {
            char time_str[64];
            time_t now = time(NULL);
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));
            fprintf(corr->csv_file, "# Phoenix SDR BCD Correlator Log v%s\n", PHOENIX_VERSION_FULL);
            fprintf(corr->csv_file, "# Started: %s\n", time_str);
            fprintf(corr->csv_file, "# Dual-path correlation: time (256-pt FFT) + freq (2048-pt FFT)\n");
            fprintf(corr->csv_file, "time,timestamp_ms,symbol_num,symbol,source,duration_ms,confidence,interval_sec,state\n");
            fflush(corr->csv_file);
        }
    }

    printf("[BCD_CORR] Correlator created: window=%.0fms, dur_tol=%.0f%%\n",
           BCD_CORR_WINDOW_MS, BCD_CORR_DURATION_TOL * 100.0f);

    return corr;
}

void bcd_correlator_destroy(bcd_correlator_t *corr) {
    if (!corr) return;

    if (corr->csv_file) fclose(corr->csv_file);
    free(corr);
}

void bcd_correlator_set_callback(bcd_correlator_t *corr,
                                 bcd_corr_symbol_callback_fn callback,
                                 void *user_data) {
    if (!corr) return;
    corr->callback = callback;
    corr->callback_user_data = user_data;
}

void bcd_correlator_time_event(bcd_correlator_t *corr,
                               float timestamp_ms,
                               float duration_ms,
                               float peak_energy) {
    if (!corr) return;

    /* Check timeout on existing pending events first */
    check_timeout(corr, timestamp_ms);

    /* Store new event */
    corr->pending_time_ms = timestamp_ms;
    corr->pending_time_duration_ms = duration_ms;
    corr->pending_time_energy = peak_energy;
    corr->time_pending = true;

    /* Try to correlate */
    try_correlate(corr, timestamp_ms);
}

void bcd_correlator_freq_event(bcd_correlator_t *corr,
                               float timestamp_ms,
                               float duration_ms,
                               float accum_energy) {
    if (!corr) return;

    /* Check timeout on existing pending events first */
    check_timeout(corr, timestamp_ms);

    /* Store new event */
    corr->pending_freq_ms = timestamp_ms;
    corr->pending_freq_duration_ms = duration_ms;
    corr->pending_freq_energy = accum_energy;
    corr->freq_pending = true;

    /* Try to correlate */
    try_correlate(corr, timestamp_ms);
}

bcd_corr_state_t bcd_correlator_get_state(bcd_correlator_t *corr) {
    return corr ? corr->state : BCD_CORR_ACQUIRING;
}

const char *bcd_corr_state_name(bcd_corr_state_t state) {
    switch (state) {
        case BCD_CORR_ACQUIRING: return "ACQUIRING";
        case BCD_CORR_TENTATIVE: return "TENTATIVE";
        case BCD_CORR_TRACKING:  return "TRACKING";
        default:                 return "UNKNOWN";
    }
}

char bcd_corr_symbol_char(bcd_corr_symbol_t sym) {
    switch (sym) {
        case BCD_CORR_SYM_ZERO:   return '0';
        case BCD_CORR_SYM_ONE:    return '1';
        case BCD_CORR_SYM_MARKER: return 'P';
        default:                  return '?';
    }
}

float bcd_correlator_get_last_symbol_ms(bcd_correlator_t *corr) {
    return corr ? corr->last_symbol_ms : 0.0f;
}

int bcd_correlator_get_symbol_count(bcd_correlator_t *corr) {
    return corr ? corr->symbol_count : 0;
}

void bcd_correlator_print_stats(bcd_correlator_t *corr) {
    if (!corr) return;

    printf("\n=== BCD CORRELATOR STATS ===\n");
    printf("State: %s\n", bcd_corr_state_name(corr->state));
    printf("Symbols confirmed: %d\n", corr->symbol_count);
    printf("Good intervals (~1s): %d\n", corr->good_intervals);
    printf("Last symbol at: %.1fms\n", corr->last_symbol_ms);
    printf("============================\n");
}
