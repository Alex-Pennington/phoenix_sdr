/**
 * @file bcd_correlator.c
 * @brief WWV BCD Window-Based Symbol Demodulator
 *
 * ARCHITECTURE (v2 - Window-Based):
 *   - Gates on sync_detector LOCKED state
 *   - Uses minute anchor to define 1-second windows
 *   - Integrates energy from time/freq detectors over each window
 *   - Classifies ONCE per window at window close
 *   - Emits exactly 60 symbols per minute (one per second)
 *
 * Signal Flow:
 *   sync_detector (LOCKED) provides anchor_ms
 *   → second boundaries: anchor + 0s, anchor + 1s, ... anchor + 59s
 *   → time_detector events accumulate energy in current window
 *   → freq_detector events accumulate energy in current window
 *   → at window close: integrate, classify, emit ONE symbol
 *
 * Pattern: Follows sync_detector.c state machine structure
 */

#include "bcd_correlator.h"
#include "sync_detector.h"
#include "version.h"
#include "waterfall_telemetry.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

/*============================================================================
 * Configuration
 *============================================================================*/

/* Window timing */
#define WINDOW_DURATION_MS      1000.0f     /* 1 second per symbol */
#define WINDOW_TOLERANCE_MS     50.0f       /* Tolerance for boundary detection */

/* Phase 8: Valid P-marker positions (WWV BCD time code format) */
static const int VALID_P_POSITIONS[] = {0, 9, 19, 29, 39, 49, 59, -1};

/* Energy integration thresholds */
#define MIN_EVENTS_FOR_SYMBOL   2           /* Need at least 2 events to trust */
#define ENERGY_THRESHOLD_LOW    0.001f      /* Below this = no signal */

/* Pulse width estimation from energy profile */
#define ENERGY_PER_MS           0.00001f    /* Rough calibration factor */

/*============================================================================
 * Internal State
 *============================================================================*/

struct bcd_correlator {
    /* Sync source - provides timing reference */
    sync_detector_t *sync_source;

    /* Current window state */
    bool window_open;               /* Is a window currently active? */
    int current_second;             /* Which second (0-59) */
    float window_start_ms;          /* Timestamp when window opened */
    float window_anchor_ms;         /* Minute anchor this window is based on */

    /* Energy accumulation for current window */
    float time_energy_sum;          /* Sum of energy from time detector */
    float time_duration_sum;        /* Sum of durations from time detector */
    int time_event_count;           /* How many time events in window */
    float time_first_ms;            /* First event timestamp in window */
    float time_last_ms;             /* Last event timestamp in window */

    float freq_energy_sum;          /* Sum of energy from freq detector */
    float freq_duration_sum;        /* Sum of durations from freq detector */
    int freq_event_count;           /* How many freq events in window */
    float freq_first_ms;            /* First event timestamp in window */
    float freq_last_ms;             /* Last event timestamp in window */

    /* Symbol tracking */
    float last_symbol_ms;           /* Timestamp of last emitted symbol */
    int symbol_count;               /* Total symbols emitted */
    int good_intervals;             /* Count of ~1s intervals */

    /* State machine */
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
 * Get the current minute anchor from sync detector
 * Returns -1 if sync is not locked
 */
static float get_minute_anchor(bcd_correlator_t *corr) {
    if (!corr->sync_source) return -1.0f;
    if (sync_detector_get_state(corr->sync_source) != SYNC_LOCKED) return -1.0f;
    return sync_detector_get_last_marker_ms(corr->sync_source);
}

/**
 * Calculate which second (0-59) a timestamp falls into
 * Returns -1 if cannot determine (sync not locked or out of range)
 */
static int get_second_for_timestamp(bcd_correlator_t *corr, float timestamp_ms, float anchor_ms) {
    if (anchor_ms < 0) return -1;

    float offset_ms = timestamp_ms - anchor_ms;

    /* Handle wrap-around for new minute */
    while (offset_ms < 0) offset_ms += 60000.0f;
    while (offset_ms >= 60000.0f) offset_ms -= 60000.0f;

    int second = (int)(offset_ms / WINDOW_DURATION_MS);
    if (second < 0) second = 0;
    if (second > 59) second = 59;

    return second;
}

/**
 * Calculate window start time for a given second
 */
static float get_window_start(float anchor_ms, int second) {
    return anchor_ms + (second * WINDOW_DURATION_MS);
}

/**
 * Estimate pulse duration from accumulated events
 * Uses the span of event timestamps as a proxy for pulse width
 */
static float estimate_pulse_duration(bcd_correlator_t *corr) {
    float time_span = 0.0f;
    float freq_span = 0.0f;

    if (corr->time_event_count >= 2) {
        time_span = corr->time_last_ms - corr->time_first_ms;
    } else if (corr->time_event_count == 1) {
        /* Single event - use reported duration */
        time_span = corr->time_duration_sum;
    }

    if (corr->freq_event_count >= 2) {
        freq_span = corr->freq_last_ms - corr->freq_first_ms;
    } else if (corr->freq_event_count == 1) {
        freq_span = corr->freq_duration_sum;
    }

    /* If we have both, average them; otherwise use whichever we have */
    if (time_span > 0 && freq_span > 0) {
        return (time_span + freq_span) / 2.0f;
    } else if (time_span > 0) {
        return time_span;
    } else if (freq_span > 0) {
        return freq_span;
    }

    /* Fallback: estimate from average durations reported */
    float avg_dur = 0.0f;
    int count = 0;
    if (corr->time_event_count > 0) {
        avg_dur += corr->time_duration_sum / corr->time_event_count;
        count++;
    }
    if (corr->freq_event_count > 0) {
        avg_dur += corr->freq_duration_sum / corr->freq_event_count;
        count++;
    }

    return (count > 0) ? avg_dur / count : 0.0f;
}

/**
 * Phase 8: Check if second position is valid for P-marker
 */
static bool is_valid_p_position(int second) {
    for (int i = 0; VALID_P_POSITIONS[i] >= 0; i++) {
        if (VALID_P_POSITIONS[i] == second) return true;
    }
    return false;
}

/**
 * Classify pulse duration into symbol type
 * Phase 8: Apply position gating for P-markers
 */
static bcd_corr_symbol_t classify_duration(float duration_ms, int second) {
    if (duration_ms < 100.0f) {
        return BCD_CORR_SYM_NONE;  /* Too short - no signal */
    } else if (duration_ms <= BCD_SYMBOL_ZERO_MAX_MS) {
        return BCD_CORR_SYM_ZERO;  /* 100-350ms = binary 0 */
    } else if (duration_ms <= BCD_SYMBOL_ONE_MAX_MS) {
        return BCD_CORR_SYM_ONE;   /* 350-650ms = binary 1 */
    } else if (duration_ms <= BCD_SYMBOL_MARKER_MAX_MS) {
        /* Phase 8: Position gating - only call it P-marker if at valid position */
        if (is_valid_p_position(second)) {
            return BCD_CORR_SYM_MARKER; /* 650-900ms = position marker */
        } else {
            return BCD_CORR_SYM_ONE;  /* Downgrade to ONE if wrong position */
        }
    }
    /* >900ms at valid position = marker, otherwise ONE */
    return is_valid_p_position(second) ? BCD_CORR_SYM_MARKER : BCD_CORR_SYM_ONE;
}

/**
 * Open a new integration window
 */
static void open_window(bcd_correlator_t *corr, int second, float anchor_ms) {
    corr->window_open = true;
    corr->current_second = second;
    corr->window_start_ms = get_window_start(anchor_ms, second);
    corr->window_anchor_ms = anchor_ms;

    /* Reset accumulators */
    corr->time_energy_sum = 0.0f;
    corr->time_duration_sum = 0.0f;
    corr->time_event_count = 0;
    corr->time_first_ms = 0.0f;
    corr->time_last_ms = 0.0f;

    corr->freq_energy_sum = 0.0f;
    corr->freq_duration_sum = 0.0f;
    corr->freq_event_count = 0;
    corr->freq_first_ms = 0.0f;
    corr->freq_last_ms = 0.0f;
}

/**
 * Close current window and emit symbol
 */
static void close_window(bcd_correlator_t *corr) {
    if (!corr->window_open) return;

    int total_events = corr->time_event_count + corr->freq_event_count;
    float total_energy = corr->time_energy_sum + corr->freq_energy_sum;

    /* Determine confidence and source */
    float confidence = 0.0f;
    const char *source = "NONE";

    if (corr->time_event_count > 0 && corr->freq_event_count > 0) {
        source = "BOTH";
        confidence = 1.0f;
    } else if (corr->time_event_count > 0) {
        source = "TIME";
        confidence = 0.6f;
    } else if (corr->freq_event_count > 0) {
        source = "FREQ";
        confidence = 0.6f;
    }

    /* Estimate pulse duration */
    float duration_ms = estimate_pulse_duration(corr);

    /* Classify symbol (Phase 8: with position gating) */
    bcd_corr_symbol_t symbol = BCD_CORR_SYM_NONE;

    if (total_events >= MIN_EVENTS_FOR_SYMBOL && total_energy > ENERGY_THRESHOLD_LOW) {
        symbol = classify_duration(duration_ms, corr->current_second);
    } else if (total_events > 0) {
        /* Some events but not enough confidence - still classify but lower confidence */
        symbol = classify_duration(duration_ms, corr->current_second);
        confidence *= 0.5f;
    }
    /* If no events at all, symbol stays NONE (no 100Hz detected this second) */

    /* Calculate timestamp for this symbol (center of window) */
    float symbol_timestamp_ms = corr->window_start_ms + (WINDOW_DURATION_MS / 2.0f);

    /* Track intervals */
    float interval_ms = 0.0f;
    if (corr->last_symbol_ms > 0) {
        interval_ms = symbol_timestamp_ms - corr->last_symbol_ms;
        if (interval_ms >= 900.0f && interval_ms <= 1100.0f) {
            corr->good_intervals++;
        }
    }

    /* Update state machine */
    if (corr->good_intervals >= 3) {
        corr->state = BCD_CORR_TRACKING;
    } else if (corr->symbol_count >= 1) {
        corr->state = BCD_CORR_TENTATIVE;
    }

    /* Update tracking */
    corr->last_symbol_ms = symbol_timestamp_ms;
    corr->symbol_count++;

    /* Log to CSV and telemetry */
    char time_str[16];
    get_wall_time_str(corr, symbol_timestamp_ms, time_str, sizeof(time_str));

    if (corr->csv_file) {
        fprintf(corr->csv_file, "%s,%.1f,%d,%d,%c,%s,%.0f,%.2f,%.1f,%d,%d,%.4f,%.4f,%s\n",
                time_str, symbol_timestamp_ms, corr->symbol_count, corr->current_second,
                bcd_corr_symbol_char(symbol), source,
                duration_ms, confidence, interval_ms / 1000.0f,
                corr->time_event_count, corr->freq_event_count,
                corr->time_energy_sum, corr->freq_energy_sum,
                bcd_corr_state_name(corr->state));
        fflush(corr->csv_file);
    }

    /* UDP telemetry for correlation stats */
    telem_sendf(TELEM_BCDS, "CORR,%s,%.1f,%d,%d,%c,%s,%.0f,%.2f,%.1f,%d,%d,%.4f,%.4f,%s",
                time_str, symbol_timestamp_ms, corr->symbol_count, corr->current_second,
                bcd_corr_symbol_char(symbol), source,
                duration_ms, confidence, interval_ms / 1000.0f,
                corr->time_event_count, corr->freq_event_count,
                corr->time_energy_sum, corr->freq_energy_sum,
                bcd_corr_state_name(corr->state));

    /* Only emit if we detected something */
    if (symbol != BCD_CORR_SYM_NONE) {
        /* Step 9: UDP telemetry with second position and confidence */
        telem_sendf(TELEM_BCDS, "SYM,%c,%d,%.0f,%.2f",
                    bcd_corr_symbol_char(symbol),
                    corr->current_second,
                    duration_ms,
                    confidence);

        /* Console output (verbose during development) */
        printf("[BCD] Sec %02d: '%c' dur=%.0fms conf=%.2f src=%s events=%d+%d state=%s\n",
               corr->current_second, bcd_corr_symbol_char(symbol),
               duration_ms, confidence, source,
               corr->time_event_count, corr->freq_event_count,
               bcd_corr_state_name(corr->state));

        /* Callback */
        if (corr->callback) {
            bcd_symbol_event_t event = {
                .symbol = symbol,
                .timestamp_ms = symbol_timestamp_ms,
                .duration_ms = duration_ms,
                .confidence = confidence,
                .source = source
            };
            corr->callback(&event, corr->callback_user_data);
        }
    }

    /* Mark window closed */
    corr->window_open = false;
}

/**
 * Check if we need to close current window and open new one
 */
static void check_window_transition(bcd_correlator_t *corr, float timestamp_ms) {
    float anchor_ms = get_minute_anchor(corr);
    if (anchor_ms < 0) {
        /* Sync not locked - close any open window and wait */
        if (corr->window_open) {
            printf("[BCD] Sync lost - closing window\n");
            close_window(corr);
        }
        return;
    }

    int new_second = get_second_for_timestamp(corr, timestamp_ms, anchor_ms);
    if (new_second < 0) return;

    if (!corr->window_open) {
        /* No window open - open one */
        open_window(corr, new_second, anchor_ms);
    } else if (new_second != corr->current_second) {
        /* Moved to new second - close current and open new */
        close_window(corr);
        open_window(corr, new_second, anchor_ms);
    }
    /* else: still in same second, keep accumulating */
}

/*============================================================================
 * Public API
 *============================================================================*/

bcd_correlator_t *bcd_correlator_create(const char *csv_path) {
    bcd_correlator_t *corr = (bcd_correlator_t *)calloc(1, sizeof(bcd_correlator_t));
    if (!corr) return NULL;

    corr->state = BCD_CORR_ACQUIRING;
    corr->start_time = time(NULL);
    corr->window_open = false;

    if (csv_path) {
        corr->csv_file = fopen(csv_path, "w");
        if (corr->csv_file) {
            char time_str[64];
            time_t now = time(NULL);
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));
            fprintf(corr->csv_file, "# Phoenix SDR BCD Correlator Log v%s\n", PHOENIX_VERSION_FULL);
            fprintf(corr->csv_file, "# Started: %s\n", time_str);
            fprintf(corr->csv_file, "# Window-based integration: 1-second windows gated on sync LOCKED\n");
            fprintf(corr->csv_file, "time,timestamp_ms,symbol_num,second,symbol,source,duration_ms,confidence,interval_sec,time_events,freq_events,time_energy,freq_energy,state\n");
            fflush(corr->csv_file);
        }
    }

    printf("[BCD] Window-based correlator created (waits for sync LOCKED)\n");

    return corr;
}

void bcd_correlator_destroy(bcd_correlator_t *corr) {
    if (!corr) return;

    /* Close any open window */
    if (corr->window_open) {
        close_window(corr);
    }

    if (corr->csv_file) fclose(corr->csv_file);
    free(corr);
}

void bcd_correlator_set_sync_source(bcd_correlator_t *corr, sync_detector_t *sync) {
    if (!corr) return;
    corr->sync_source = sync;
    printf("[BCD] Sync source linked - will gate on LOCKED state\n");
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

    /* Check for window transition first */
    check_window_transition(corr, timestamp_ms);

    /* If no window open (sync not locked), ignore event */
    if (!corr->window_open) return;

    /* Accumulate into current window */
    if (corr->time_event_count == 0) {
        corr->time_first_ms = timestamp_ms;
    }
    corr->time_last_ms = timestamp_ms;
    corr->time_energy_sum += peak_energy;
    corr->time_duration_sum += duration_ms;
    corr->time_event_count++;
}

void bcd_correlator_freq_event(bcd_correlator_t *corr,
                               float timestamp_ms,
                               float duration_ms,
                               float accum_energy) {
    if (!corr) return;

    /* Check for window transition first */
    check_window_transition(corr, timestamp_ms);

    /* If no window open (sync not locked), ignore event */
    if (!corr->window_open) return;

    /* Accumulate into current window */
    if (corr->freq_event_count == 0) {
        corr->freq_first_ms = timestamp_ms;
    }
    corr->freq_last_ms = timestamp_ms;
    corr->freq_energy_sum += accum_energy;
    corr->freq_duration_sum += duration_ms;
    corr->freq_event_count++;
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
        default:                  return '.';  /* No signal = dot */
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
    printf("Mode: Window-based (1-second integration)\n");
    printf("Sync source: %s\n", corr->sync_source ? "linked" : "NOT LINKED");
    printf("State: %s\n", bcd_corr_state_name(corr->state));
    printf("Symbols emitted: %d\n", corr->symbol_count);
    printf("Good intervals (~1s): %d\n", corr->good_intervals);
    printf("Last symbol at: %.1fms\n", corr->last_symbol_ms);
    printf("Current window: %s (second %d)\n",
           corr->window_open ? "OPEN" : "CLOSED", corr->current_second);
    printf("============================\n");
}
