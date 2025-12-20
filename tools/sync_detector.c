/**
 * @file sync_detector.c
 * @brief WWV unified sync detector implementation
 *
 * Fuses evidence from multiple sources (ticks, markers, P-markers, tick-holes)
 * to provide authoritative frame timing with confidence tracking and recovery.
 */

#include "sync_detector.h"
#include "wwv_clock.h"
#include "version.h"
#include "waterfall_telemetry.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

/*============================================================================
 * Configuration - from spec
 *============================================================================*/

/* Legacy correlation (backward compat) */
#define CORRELATION_WINDOW_MS   1500.0f
#define MARKER_INTERVAL_MIN_MS  55000.0f
#define MARKER_INTERVAL_MAX_MS  65000.0f
#define MARKER_NOMINAL_MS       60000.0f
#define PENDING_TIMEOUT_MS      3000.0f
#define SYNC_FLASH_FRAMES       30

/* Timing thresholds */
#define TICK_INTERVAL_MIN_MS         950.0f
#define TICK_INTERVAL_MAX_MS         1050.0f
#define TICK_HOLE_MIN_GAP_MS         1700.0f
#define TICK_HOLE_MAX_GAP_MS         2200.0f
#define TICK_DOUBLE_MAX_GAP_MS       100.0f

/* Marker-based signal health (markers reliable even on weak signals) */
#define MARKER_GAP_WARNING_MS        75000.0f  /* 1.25 marker intervals */
#define MARKER_GAP_CRITICAL_MS       90000.0f  /* 1.5 marker intervals = signal lost */

#define SYNC_RETENTION_WINDOW_MS     120000.0f
#define SYNC_RECOVERY_TIMEOUT_MS     10000.0f

/* Evidence weights */
#define WEIGHT_TICK                  0.05f
#define WEIGHT_MARKER                0.40f
#define WEIGHT_P_MARKER              0.15f
#define WEIGHT_TICK_HOLE             0.20f
#define WEIGHT_COMBINED_HOLE_MARKER  0.50f

/* Confidence thresholds */
#define CONFIDENCE_LOCKED_THRESHOLD  0.70f
#define CONFIDENCE_MIN_RETAIN        0.05f
#define CONFIDENCE_TENTATIVE_INIT    0.30f
/* Changed from 0.995f (95% decay in 60s) to 0.9999f (6% decay in 60s) - allows accumulation */
#define CONFIDENCE_DECAY_NORMAL      0.9999f
#define CONFIDENCE_DECAY_RECOVERING  0.980f

/* Validation tolerances */
#define TICK_PHASE_TOLERANCE_MS      100.0f
#define MARKER_TOLERANCE_MS          500.0f
#define P_MARKER_TOLERANCE_MS        200.0f
#define LEAP_SECOND_EXTRA_MS         1000.0f

/* Debounce */
#define MIN_TICKS_FOR_HOLE           20
#define SIGNAL_WEAK_DEBOUNCE         3

/*============================================================================
 * Internal State
 *============================================================================*/

typedef struct {
    int consecutive_tick_count;
    float last_tick_ms;
    float prev_hole_ms;
    float last_hole_ms;
    int hole_count;
} tick_gap_tracker_t;

typedef struct {
    float retained_anchor_ms;
    float signal_lost_ms;
    float recovery_start_ms;
    bool has_retained_state;
    bool recovery_tick_seen;
    bool recovery_marker_seen;
    bool recovery_p_marker_seen;
} recovery_state_t;

struct sync_detector {
    /* Legacy pending events (backward compat) */
    float pending_tick_ms;
    float pending_tick_duration_ms;
    float pending_tick_corr_ratio;
    bool tick_pending;
    float pending_marker_ms;
    float pending_marker_energy;
    float pending_marker_duration_ms;
    bool marker_pending;

    /* Confirmed markers */
    float last_confirmed_ms;
    float prev_confirmed_ms;
    int confirmed_count;
    int good_intervals;

    /* Enhanced state */
    sync_state_t state;
    float confidence;
    uint32_t evidence_mask;
    float minute_anchor_ms;         /* Authoritative minute boundary */

    /* Tick gap tracking */
    tick_gap_tracker_t tick_gap;

    /* Recovery state */
    recovery_state_t recovery;

    /* Signal loss detection */
    int signal_weak_count;
    bool expecting_marker_soon;
    float expected_marker_ms;

    /* Optional integrations */
    wwv_clock_t *wwv_clock;
    bool leap_second_pending;

    /* Tunable parameters (runtime adjustable via UDP commands) */
    float weight_tick;                      /* Tick evidence weight (0.01-0.2, default 0.05) */
    float weight_marker;                    /* Marker evidence weight (0.1-0.6, default 0.40) */
    float weight_p_marker;                  /* P-marker evidence weight (0.05-0.3, default 0.15) */
    float weight_tick_hole;                 /* Tick hole evidence weight (0.05-0.4, default 0.20) */
    float weight_combined_hole_marker;      /* Combined hole+marker weight (0.2-0.8, default 0.50) */
    float confidence_locked_threshold;      /* Threshold to reach LOCKED (0.5-0.9, default 0.70) */
    float confidence_min_retain;            /* Minimum to keep state (0.01-0.2, default 0.05) */
    float confidence_tentative_init;        /* Initial tentative confidence (0.1-0.5, default 0.30) */
    float confidence_decay_normal;          /* Normal decay rate (0.99-0.9999, default 0.9999) */
    float confidence_decay_recovering;      /* Recovery mode decay (0.90-0.99, default 0.980) */
    float tick_phase_tolerance_ms;          /* Tick timing tolerance (50.0-200.0, default 100.0) */
    float marker_tolerance_ms;              /* Marker timing tolerance (200.0-800.0, default 500.0) */
    float p_marker_tolerance_ms;            /* P-marker timing tolerance (100.0-400.0, default 200.0) */

    /* Callbacks */
    sync_state_callback_fn state_callback;
    void *state_callback_user_data;

    /* UI feedback */
    int flash_frames_remaining;

    /* Logging */
    FILE *csv_file;
    time_t start_time;
};

/*============================================================================
 * Internal Functions
 *============================================================================*/

/* Forward declarations */
static void transition_state(sync_detector_t *sd, sync_state_t new_state);
static void apply_evidence(sync_detector_t *sd, uint32_t evidence_type, float weight);
static float get_evidence_weight(sync_detector_t *sd, uint32_t evidence_type);
static void sync_detector_hole_detected(sync_detector_t *sd, float hole_timestamp_ms);
static void sync_detector_full_reset(sync_detector_t *sd);

static void get_wall_time_str(sync_detector_t *sd, float timestamp_ms, char *buf, size_t buflen) {
    time_t event_time = sd->start_time + (time_t)(timestamp_ms / 1000.0f);
    struct tm *tm_info = localtime(&event_time);
    strftime(buf, buflen, "%H:%M:%S", tm_info);
}

static void log_confirmed_marker(sync_detector_t *sd, float timestamp_ms,
                                  float interval_ms, float delta_ms,
                                  const char *source) {
    if (!sd->csv_file) return;

    char time_str[16];
    get_wall_time_str(sd, timestamp_ms, time_str, sizeof(time_str));

    fprintf(sd->csv_file, "%s,%.1f,%d,%s,%.1f,%.0f,%.1f,%.1f\n",
            time_str,
            timestamp_ms,
            sd->confirmed_count,
            sync_state_name(sd->state),
            interval_ms / 1000.0f,
            delta_ms,
            sd->pending_tick_duration_ms,
            sd->pending_marker_duration_ms);
    fflush(sd->csv_file);

    /* UDP telemetry broadcast - expanded format */
    telem_sendf(TELEM_SYNC, "%s,%.1f,%d,%s,%d,%.1f,%.0f,%.1f,%.1f,%.1f",
                time_str, timestamp_ms, sd->confirmed_count,
                sync_state_name(sd->state), sd->good_intervals,
                interval_ms / 1000.0f, delta_ms,
                sd->pending_tick_duration_ms, sd->pending_marker_duration_ms,
                sd->last_confirmed_ms);
}

static void confirm_marker(sync_detector_t *sd, float marker_time, float delta_ms, const char *source) {
    /* Calculate interval from previous marker */
    float interval_ms = (sd->last_confirmed_ms > 0) ?
        (marker_time - sd->last_confirmed_ms) : 0.0f;

    /* Check if interval is a valid multiple of ~60 seconds (allows for missed markers) */
    bool good_interval = (sd->last_confirmed_ms == 0);
    if (!good_interval && interval_ms >= MARKER_INTERVAL_MIN_MS) {
        /* Find how many 60-second periods fit */
        int periods = (int)((interval_ms + MARKER_NOMINAL_MS/2) / MARKER_NOMINAL_MS);
        float expected_ms = periods * MARKER_NOMINAL_MS;
        float error_ms = fabsf(interval_ms - expected_ms);
        /* Accept if within ±5 seconds of a multiple of 60s */
        good_interval = (error_ms <= 5000.0f);
    }

    if (good_interval) {
        /* Update confirmed marker history */
        sd->prev_confirmed_ms = sd->last_confirmed_ms;
        sd->last_confirmed_ms = marker_time;
        sd->minute_anchor_ms = marker_time;  /* Set authoritative anchor */
        sd->confirmed_count++;

        /* Track good intervals for lock confidence */
        if (interval_ms >= MARKER_INTERVAL_MIN_MS &&
            interval_ms <= MARKER_INTERVAL_MAX_MS) {
            sd->good_intervals++;
        }

        /* Apply marker evidence */
        float weight = get_evidence_weight(sd, EVIDENCE_MARKER);

        /* Check if marker confirms :59 tick hole */
        if (sd->expecting_marker_soon) {
            float delta_from_expected = fabsf(marker_time - sd->expected_marker_ms);
            if (delta_from_expected < 200.0f) {
                printf("[SYNC] Marker confirms :59 tick hole - high confidence\n");
                weight = WEIGHT_COMBINED_HOLE_MARKER;
            }
            sd->expecting_marker_soon = false;
        }

        apply_evidence(sd, EVIDENCE_MARKER, weight);

        /* Legacy state transitions */
        if (sd->state == SYNC_ACQUIRING) {
            transition_state(sd, SYNC_TENTATIVE);
        }

        /* Flash for UI */
        sd->flash_frames_remaining = SYNC_FLASH_FRAMES;

        /* Log to CSV */
        log_confirmed_marker(sd, marker_time, interval_ms, delta_ms, source);

        /* Console output */
        printf("[SYNC] *** CONFIRMED MARKER #%d *** src=%s delta=%.0fms interval=%.1fs state=%s conf=%.2f\n",
               sd->confirmed_count, source, delta_ms, interval_ms / 1000.0f,
               sync_state_name(sd->state), sd->confidence);
    } else {
        printf("[SYNC] Marker rejected (%s): interval=%.1fs (out of range)\n",
               source, interval_ms / 1000.0f);
    }

    /* Clear both pending events */
    sd->tick_pending = false;
    sd->marker_pending = false;
}

static void try_correlate(sync_detector_t *sd, float current_ms) {
    /* Accept EITHER detector firing - no longer require both */

    /* If both are pending, check correlation and use the better source */
    if (sd->tick_pending && sd->marker_pending) {
        float delta = fabsf(sd->pending_marker_ms - sd->pending_tick_ms);
        if (delta < CORRELATION_WINDOW_MS) {
            /* Both detectors agree - use tick detector timestamp (more precise) */
            confirm_marker(sd, sd->pending_tick_ms, delta, "BOTH");
            return;
        }
        /* If they don't correlate, prefer the earlier one */
        if (sd->pending_tick_ms < sd->pending_marker_ms) {
            confirm_marker(sd, sd->pending_tick_ms, 0.0f, "TICK");
        } else {
            confirm_marker(sd, sd->pending_marker_ms, 0.0f, "MARK");
        }
        return;
    }

    /* Single detector fired - no correlation partner available yet */
    /* The timeout handler will confirm if partner doesn't arrive */
}

static void check_timeout(sync_detector_t *sd, float current_ms) {
    /* If a single detector fired and partner didn't arrive, confirm from single source */
    if (sd->tick_pending && !sd->marker_pending &&
        (current_ms - sd->pending_tick_ms) > PENDING_TIMEOUT_MS) {
        confirm_marker(sd, sd->pending_tick_ms, 0.0f, "TICK");
        return;
    }
    if (sd->marker_pending && !sd->tick_pending &&
        (current_ms - sd->pending_marker_ms) > PENDING_TIMEOUT_MS) {
        confirm_marker(sd, sd->pending_marker_ms, 0.0f, "MARK");
        return;
    }
}

/*============================================================================
 * Enhanced Sync Functions
 *============================================================================*/

static float get_evidence_weight(sync_detector_t *sd, uint32_t evidence_type) {
    float base_weight;

    switch (evidence_type) {
        case EVIDENCE_TICK:       base_weight = sd->weight_tick; break;
        case EVIDENCE_MARKER:     base_weight = sd->weight_marker; break;
        case EVIDENCE_P_MARKER:   base_weight = sd->weight_p_marker; break;
        case EVIDENCE_TICK_HOLE:  base_weight = sd->weight_tick_hole; break;
        default:                  base_weight = 0.10f; break;
    }

    /* Schedule-aware discount if wwv_clock available */
    if (sd->wwv_clock && wwv_clock_is_special_minute(sd->wwv_clock)) {
        base_weight *= 0.5f;
    }

    return base_weight;
}

static void apply_evidence(sync_detector_t *sd, uint32_t evidence_type, float weight) {
    sd->evidence_mask |= evidence_type;

    /* Boost confidence asymptotically toward 1.0 */
    sd->confidence += weight * (1.0f - sd->confidence);
    if (sd->confidence > 1.0f) sd->confidence = 1.0f;

    /* State transitions based on confidence */
    if (sd->state == SYNC_TENTATIVE && sd->confidence >= sd->confidence_locked_threshold) {
        transition_state(sd, SYNC_LOCKED);
    }
}

static void transition_state(sync_detector_t *sd, sync_state_t new_state) {
    if (sd->state == new_state) return;

    sync_state_t old_state = sd->state;
    sd->state = new_state;

    /* State-specific initialization */
    switch (new_state) {
        case SYNC_ACQUIRING:
            sd->confidence = 0.0f;
            break;
        case SYNC_TENTATIVE:
            if (sd->confidence < CONFIDENCE_TENTATIVE_INIT) {
                sd->confidence = CONFIDENCE_TENTATIVE_INIT;
            }
            break;
        case SYNC_LOCKED:
            /* Confidence already above threshold */
            break;
        case SYNC_RECOVERING:
            /* Confidence retained, will decay faster */
            sd->recovery.recovery_start_ms = sd->tick_gap.last_tick_ms;
            break;
    }

    printf("[SYNC] State transition: %s → %s (conf=%.2f)\n",
           sync_state_name(old_state), sync_state_name(new_state), sd->confidence);

    /* UDP telemetry */
    telem_sendf(TELEM_SYNC, "STATE,%s,%s,%.2f",
                sync_state_name(old_state), sync_state_name(new_state), sd->confidence);

    /* User callback */
    if (sd->state_callback) {
        sd->state_callback(old_state, new_state, sd->confidence, sd->state_callback_user_data);
    }
}

static void sync_detector_hole_detected(sync_detector_t *sd, float hole_timestamp_ms) {
    int probable_second = -1;

    /* Determine position if locked */
    if (sd->state == SYNC_LOCKED && sd->minute_anchor_ms > 0) {
        float since_anchor = hole_timestamp_ms - sd->minute_anchor_ms;
        while (since_anchor < 0) since_anchor += 60000.0f;
        while (since_anchor >= 60000.0f) since_anchor -= 60000.0f;

        int position = (int)(since_anchor / 1000.0f + 0.5f);

        if (abs(position - 29) <= 1) {
            probable_second = 29;
        } else if (abs(position - 59) <= 1 || position <= 1) {
            probable_second = 59;
            sd->expecting_marker_soon = true;
            sd->expected_marker_ms = hole_timestamp_ms + 1000.0f;
        }
    }

    float weight = get_evidence_weight(sd, EVIDENCE_TICK_HOLE);

    if (probable_second >= 0) {
        printf("[SYNC] Tick hole at second %d (%.0fms)\n", probable_second, hole_timestamp_ms);
        apply_evidence(sd, EVIDENCE_TICK_HOLE, weight);
    } else if (sd->state == SYNC_ACQUIRING) {
        /* During acquisition, look for double-hole pattern */
        sd->tick_gap.hole_count++;
        if (sd->tick_gap.hole_count >= 2 && sd->tick_gap.prev_hole_ms > 0) {
            float hole_interval = hole_timestamp_ms - sd->tick_gap.prev_hole_ms;
            if (fabsf(hole_interval - 30000.0f) < 500.0f) {
                printf("[SYNC] Double tick-hole pattern confirmed (%.1fs apart)\n",
                       hole_interval / 1000.0f);
                apply_evidence(sd, EVIDENCE_TICK_HOLE, weight * 1.5f);
            }
        }
        sd->tick_gap.prev_hole_ms = sd->tick_gap.last_hole_ms;
        sd->tick_gap.last_hole_ms = hole_timestamp_ms;
    }
}

static void sync_detector_full_reset(sync_detector_t *sd) {
    printf("[SYNC] Full reset - clearing all state\n");

    memset(&sd->tick_gap, 0, sizeof(tick_gap_tracker_t));
    memset(&sd->recovery, 0, sizeof(recovery_state_t));

    sd->confidence = 0.0f;
    sd->evidence_mask = 0;
    sd->signal_weak_count = 0;
    sd->expecting_marker_soon = false;

    transition_state(sd, SYNC_ACQUIRING);
}

/*============================================================================
 * Public API
 *============================================================================*/

sync_detector_t *sync_detector_create(const char *csv_path) {
    sync_detector_t *sd = (sync_detector_t *)calloc(1, sizeof(sync_detector_t));
    if (!sd) return NULL;

    sd->state = SYNC_ACQUIRING;
    sd->start_time = time(NULL);

    /* Initialize tunable parameters to defaults */
    sd->weight_tick = WEIGHT_TICK;                                      /* 0.05 */
    sd->weight_marker = WEIGHT_MARKER;                                  /* 0.40 */
    sd->weight_p_marker = WEIGHT_P_MARKER;                              /* 0.15 */
    sd->weight_tick_hole = WEIGHT_TICK_HOLE;                            /* 0.20 */
    sd->weight_combined_hole_marker = WEIGHT_COMBINED_HOLE_MARKER;      /* 0.50 */
    sd->confidence_locked_threshold = CONFIDENCE_LOCKED_THRESHOLD;      /* 0.70 */
    sd->confidence_min_retain = CONFIDENCE_MIN_RETAIN;                  /* 0.05 */
    sd->confidence_tentative_init = CONFIDENCE_TENTATIVE_INIT;          /* 0.30 */
    sd->confidence_decay_normal = CONFIDENCE_DECAY_NORMAL;              /* 0.9999 */
    sd->confidence_decay_recovering = CONFIDENCE_DECAY_RECOVERING;      /* 0.980 */
    sd->tick_phase_tolerance_ms = TICK_PHASE_TOLERANCE_MS;              /* 100.0 */
    sd->marker_tolerance_ms = MARKER_TOLERANCE_MS;                      /* 500.0 */
    sd->p_marker_tolerance_ms = P_MARKER_TOLERANCE_MS;                  /* 200.0 */

    /* Open CSV file */
    if (csv_path) {
        sd->csv_file = fopen(csv_path, "w");
        if (sd->csv_file) {
            char time_str[64];
            time_t now = time(NULL);
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));

            fprintf(sd->csv_file, "# Phoenix SDR WWV Sync Log v%s\n", PHOENIX_VERSION_FULL);
            fprintf(sd->csv_file, "# Started: %s\n", time_str);
            fprintf(sd->csv_file, "time,timestamp_ms,marker_num,state,interval_sec,delta_ms,tick_dur_ms,marker_dur_ms\n");
            fflush(sd->csv_file);
        }
    }

    printf("[SYNC] Detector created, state=ACQUIRING\n");

    return sd;
}

void sync_detector_destroy(sync_detector_t *sd) {
    if (!sd) return;

    if (sd->csv_file) {
        fclose(sd->csv_file);
    }

    free(sd);
}

void sync_detector_tick_marker(sync_detector_t *sd, float timestamp_ms,
                                float duration_ms, float corr_ratio) {
    if (!sd) return;

    /* Check for timeout on previous pending events */
    check_timeout(sd, timestamp_ms);

    /* Store pending tick marker event */
    sd->pending_tick_ms = timestamp_ms;
    sd->pending_tick_duration_ms = duration_ms;
    sd->pending_tick_corr_ratio = corr_ratio;
    sd->tick_pending = true;

    printf("[SYNC] Tick marker received: %.1fms dur=%.0fms\n", timestamp_ms, duration_ms);

    /* Try to correlate with pending marker event */
    try_correlate(sd, timestamp_ms);
}

void sync_detector_marker_event(sync_detector_t *sd, float timestamp_ms,
                                 float accum_energy, float duration_ms) {
    if (!sd) return;

    /* Check for timeout on previous pending events */
    check_timeout(sd, timestamp_ms);

    /* Store pending marker event */
    sd->pending_marker_ms = timestamp_ms;
    sd->pending_marker_energy = accum_energy;
    sd->pending_marker_duration_ms = duration_ms;
    sd->marker_pending = true;

    printf("[SYNC] Marker event received: %.1fms energy=%.0f dur=%.0fms\n",
           timestamp_ms, accum_energy, duration_ms);

    /* Try to correlate with pending tick marker */
    try_correlate(sd, timestamp_ms);
}

sync_state_t sync_detector_get_state(sync_detector_t *sd) {
    return sd ? sd->state : SYNC_ACQUIRING;
}

const char *sync_state_name(sync_state_t state) {
    switch (state) {
        case SYNC_ACQUIRING:  return "ACQUIRING";
        case SYNC_TENTATIVE:  return "TENTATIVE";
        case SYNC_LOCKED:     return "LOCKED";
        case SYNC_RECOVERING: return "RECOVERING";
        default:              return "UNKNOWN";
    }
}

float sync_detector_get_last_marker_ms(sync_detector_t *sd) {
    return sd ? sd->last_confirmed_ms : 0.0f;
}

int sync_detector_get_confirmed_count(sync_detector_t *sd) {
    return sd ? sd->confirmed_count : 0;
}

int sync_detector_get_flash_frames(sync_detector_t *sd) {
    return sd ? sd->flash_frames_remaining : 0;
}

void sync_detector_decrement_flash(sync_detector_t *sd) {
    if (sd && sd->flash_frames_remaining > 0) {
        sd->flash_frames_remaining--;
    }
}

int sync_detector_get_good_intervals(sync_detector_t *sd) {
    return sd ? sd->good_intervals : 0;
}

void sync_detector_broadcast_state(sync_detector_t *sd) {
    if (!sd) return;

    char time_str[16];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);

    /* Broadcast current state - use 0 for interval/delta when no history */
    float interval_sec = 0.0f;
    if (sd->prev_confirmed_ms > 0 && sd->last_confirmed_ms > 0) {
        interval_sec = (sd->last_confirmed_ms - sd->prev_confirmed_ms) / 1000.0f;
    }

    telem_sendf(TELEM_SYNC, "%s,%.1f,%d,%s,%d,%.1f,0,%.1f,%.1f,%.1f",
                time_str, sd->last_confirmed_ms, sd->confirmed_count,
                sync_state_name(sd->state), sd->good_intervals,
                interval_sec,
                sd->pending_tick_duration_ms, sd->pending_marker_duration_ms,
                sd->last_confirmed_ms);
}

/*============================================================================
 * Enhanced API Implementation
 *============================================================================*/

void sync_detector_tick_event(sync_detector_t *sd, float timestamp_ms) {
    if (!sd) return;

    tick_gap_tracker_t *tg = &sd->tick_gap;

    if (tg->last_tick_ms > 0) {
        float gap_ms = timestamp_ms - tg->last_tick_ms;

        if (gap_ms >= TICK_INTERVAL_MIN_MS && gap_ms <= TICK_INTERVAL_MAX_MS) {
            /* Normal tick interval */
            tg->consecutive_tick_count++;

        } else if (gap_ms < TICK_DOUBLE_MAX_GAP_MS) {
            /* DUT1 double-tick - ignore, don't reset counter */
            return;

        } else if (gap_ms >= TICK_HOLE_MIN_GAP_MS && gap_ms <= TICK_HOLE_MAX_GAP_MS) {
            /* Tick hole detected */
            if (tg->consecutive_tick_count >= MIN_TICKS_FOR_HOLE) {
                sync_detector_hole_detected(sd, tg->last_tick_ms + 1000.0f);
            }
            tg->consecutive_tick_count = 1;

        } else if (gap_ms > TICK_HOLE_MAX_GAP_MS) {
            /* Signal loss */
            printf("[SYNC] Tick gap %.0fms - signal loss\n", gap_ms);
            tg->consecutive_tick_count = 1;
        } else {
            /* Short gap - noise */
            return;
        }
    } else {
        tg->consecutive_tick_count = 1;
    }

    tg->last_tick_ms = timestamp_ms;

    /* Apply tick evidence */
    if (sd->state >= SYNC_TENTATIVE) {
        float weight = get_evidence_weight(sd, EVIDENCE_TICK);
        apply_evidence(sd, EVIDENCE_TICK, weight);
    }
}

void sync_detector_p_marker_event(sync_detector_t *sd, float timestamp_ms,
                                   float duration_ms) {
    if (!sd) return;

    printf("[SYNC] P-marker: %.1fms dur=%.0fms\n", timestamp_ms, duration_ms);

    /* Apply P-marker evidence */
    if (sd->state >= SYNC_TENTATIVE) {
        float weight = get_evidence_weight(sd, EVIDENCE_P_MARKER);
        apply_evidence(sd, EVIDENCE_P_MARKER, weight);
    }

    /* Recovery validation */
    if (sd->state == SYNC_RECOVERING && sd->recovery.has_retained_state) {
        float since_anchor = timestamp_ms - sd->recovery.retained_anchor_ms;
        while (since_anchor < 0) since_anchor += 60000.0f;
        while (since_anchor >= 60000.0f) since_anchor -= 60000.0f;

        int position = (int)(since_anchor / 1000.0f + 0.5f);

        /* Check if at valid P position */
        static const int valid_p[] = {0, 9, 19, 29, 39, 49, 59};
        for (int i = 0; i < 7; i++) {
            if (abs(position - valid_p[i]) <= 1) {
                sd->recovery.recovery_p_marker_seen = true;
                printf("[SYNC] Recovery: P-marker at second %d validates anchor\n", position);
                break;
            }
        }
    }
}

void sync_detector_periodic_check(sync_detector_t *sd, float current_ms) {
    if (!sd) return;

    /* Confidence decay */
    float decay_rate = (sd->state == SYNC_RECOVERING) ?
                       sd->confidence_decay_recovering : sd->confidence_decay_normal;
    sd->confidence *= decay_rate;

    /* Signal loss detection (only when LOCKED) - marker-based authority */
    if (sd->state == SYNC_LOCKED) {
        float since_last_marker = current_ms - sd->last_confirmed_ms;

        if (since_last_marker > MARKER_GAP_CRITICAL_MS) {
            sd->signal_weak_count++;
            if (sd->signal_weak_count >= SIGNAL_WEAK_DEBOUNCE) {
                /* Enter recovery */
                sd->recovery.retained_anchor_ms = sd->minute_anchor_ms;
                sd->recovery.signal_lost_ms = current_ms;
                sd->recovery.has_retained_state = true;
                sd->recovery.recovery_start_ms = current_ms;
                sd->recovery.recovery_tick_seen = false;
                sd->recovery.recovery_marker_seen = false;
                sd->recovery.recovery_p_marker_seen = false;

                transition_state(sd, SYNC_RECOVERING);
            }
        } else {
            sd->signal_weak_count = 0;
        }
    }

    /* Recovery state processing */
    if (sd->state == SYNC_RECOVERING) {
        float time_since_loss = current_ms - sd->recovery.signal_lost_ms;
        float time_in_recovery = current_ms - sd->recovery.recovery_start_ms;

        /* Check for full reset conditions */
        if (time_since_loss > SYNC_RETENTION_WINDOW_MS ||
            sd->confidence < CONFIDENCE_MIN_RETAIN) {
            printf("[SYNC] Retention expired (%.0fs) - full reset\n", time_since_loss / 1000.0f);
            sync_detector_full_reset(sd);
            return;
        }

        /* Check recovery timeout */
        if (time_in_recovery > SYNC_RECOVERY_TIMEOUT_MS) {
            if (!sd->recovery.recovery_tick_seen && !sd->recovery.recovery_marker_seen) {
                printf("[SYNC] Recovery timeout with no validation - full reset\n");
                sync_detector_full_reset(sd);
                return;
            }
        }

        /* Check for successful recovery */
        if (sd->recovery.recovery_tick_seen && sd->recovery.recovery_marker_seen) {
            printf("[SYNC] Recovery validated - returning to LOCKED\n");
            sd->minute_anchor_ms = sd->recovery.retained_anchor_ms;
            apply_evidence(sd, EVIDENCE_TICK | EVIDENCE_MARKER, 0.3f);
            transition_state(sd, SYNC_LOCKED);
        }
    }

    /* State transition: TENTATIVE → LOCKED */
    if (sd->state == SYNC_TENTATIVE && sd->confidence >= CONFIDENCE_LOCKED_THRESHOLD) {
        transition_state(sd, SYNC_LOCKED);
    }
}

frame_time_t sync_detector_get_frame_time(sync_detector_t *sd) {
    frame_time_t ft = {0};

    if (!sd || sd->state < SYNC_TENTATIVE || sd->minute_anchor_ms <= 0) {
        ft.current_second = -1;
        ft.state = sd ? sd->state : SYNC_ACQUIRING;
        return ft;
    }

    /* Calculate current second from anchor */
    float since_anchor = sd->tick_gap.last_tick_ms - sd->minute_anchor_ms;
    while (since_anchor < 0) since_anchor += 60000.0f;
    while (since_anchor >= 60000.0f) since_anchor -= 60000.0f;

    ft.current_second = (int)(since_anchor / 1000.0f + 0.5f);
    if (ft.current_second < 0) ft.current_second = 0;
    if (ft.current_second > 59) ft.current_second = 59;

    ft.second_start_ms = sd->minute_anchor_ms + (ft.current_second * 1000.0f);
    ft.confidence = sd->confidence;
    ft.evidence_mask = sd->evidence_mask;
    ft.state = sd->state;

    return ft;
}

float sync_detector_get_confidence(sync_detector_t *sd) {
    return sd ? sd->confidence : 0.0f;
}

void sync_detector_set_wwv_clock(sync_detector_t *sd, wwv_clock_t *clk) {
    if (sd) {
        sd->wwv_clock = clk;
    }
}

void sync_detector_set_leap_second_pending(sync_detector_t *sd, bool pending) {
    if (sd) {
        sd->leap_second_pending = pending;
    }
}

void sync_detector_set_state_callback(sync_detector_t *sd,
                                       sync_state_callback_fn callback,
                                       void *user_data) {
    if (sd) {
        sd->state_callback = callback;
        sd->state_callback_user_data = user_data;
    }
}

bool sync_detector_get_pending_tick(sync_detector_t *sd, float *timestamp_ms, float *duration_ms) {
    if (!sd || !sd->tick_pending) {
        return false;
    }
    if (timestamp_ms) *timestamp_ms = sd->pending_tick_ms;
    if (duration_ms) *duration_ms = sd->pending_tick_duration_ms;
    return true;
}

/*============================================================================
 * Runtime Parameter Tuning
 *============================================================================*/

/* Evidence weights */
void sync_detector_set_weight_tick(sync_detector_t *sd, float weight) {
    if (!sd || weight < 0.01f || weight > 0.2f) return;
    sd->weight_tick = weight;
}

float sync_detector_get_weight_tick(sync_detector_t *sd) {
    return sd ? sd->weight_tick : WEIGHT_TICK;
}

void sync_detector_set_weight_marker(sync_detector_t *sd, float weight) {
    if (!sd || weight < 0.1f || weight > 0.6f) return;
    sd->weight_marker = weight;
}

float sync_detector_get_weight_marker(sync_detector_t *sd) {
    return sd ? sd->weight_marker : WEIGHT_MARKER;
}

void sync_detector_set_weight_p_marker(sync_detector_t *sd, float weight) {
    if (!sd || weight < 0.05f || weight > 0.3f) return;
    sd->weight_p_marker = weight;
}

float sync_detector_get_weight_p_marker(sync_detector_t *sd) {
    return sd ? sd->weight_p_marker : WEIGHT_P_MARKER;
}

void sync_detector_set_weight_tick_hole(sync_detector_t *sd, float weight) {
    if (!sd || weight < 0.05f || weight > 0.4f) return;
    sd->weight_tick_hole = weight;
}

float sync_detector_get_weight_tick_hole(sync_detector_t *sd) {
    return sd ? sd->weight_tick_hole : WEIGHT_TICK_HOLE;
}

void sync_detector_set_weight_combined(sync_detector_t *sd, float weight) {
    if (!sd || weight < 0.2f || weight > 0.8f) return;
    sd->weight_combined_hole_marker = weight;
}

float sync_detector_get_weight_combined(sync_detector_t *sd) {
    return sd ? sd->weight_combined_hole_marker : WEIGHT_COMBINED_HOLE_MARKER;
}

/* Confidence thresholds */
void sync_detector_set_locked_threshold(sync_detector_t *sd, float threshold) {
    if (!sd || threshold < 0.5f || threshold > 0.9f) return;
    sd->confidence_locked_threshold = threshold;
}

float sync_detector_get_locked_threshold(sync_detector_t *sd) {
    return sd ? sd->confidence_locked_threshold : CONFIDENCE_LOCKED_THRESHOLD;
}

void sync_detector_set_min_retain(sync_detector_t *sd, float threshold) {
    if (!sd || threshold < 0.01f || threshold > 0.2f) return;
    sd->confidence_min_retain = threshold;
}

float sync_detector_get_min_retain(sync_detector_t *sd) {
    return sd ? sd->confidence_min_retain : CONFIDENCE_MIN_RETAIN;
}

void sync_detector_set_tentative_init(sync_detector_t *sd, float confidence) {
    if (!sd || confidence < 0.1f || confidence > 0.5f) return;
    sd->confidence_tentative_init = confidence;
}

float sync_detector_get_tentative_init(sync_detector_t *sd) {
    return sd ? sd->confidence_tentative_init : CONFIDENCE_TENTATIVE_INIT;
}

/* Confidence decay rates */
void sync_detector_set_decay_normal(sync_detector_t *sd, float rate) {
    if (!sd || rate < 0.99f || rate > 0.9999f) return;
    sd->confidence_decay_normal = rate;
}

float sync_detector_get_decay_normal(sync_detector_t *sd) {
    return sd ? sd->confidence_decay_normal : CONFIDENCE_DECAY_NORMAL;
}

void sync_detector_set_decay_recovering(sync_detector_t *sd, float rate) {
    if (!sd || rate < 0.90f || rate > 0.99f) return;
    sd->confidence_decay_recovering = rate;
}

float sync_detector_get_decay_recovering(sync_detector_t *sd) {
    return sd ? sd->confidence_decay_recovering : CONFIDENCE_DECAY_RECOVERING;
}

/* Validation tolerances */
void sync_detector_set_tick_tolerance(sync_detector_t *sd, float ms) {
    if (!sd || ms < 50.0f || ms > 200.0f) return;
    sd->tick_phase_tolerance_ms = ms;
}

float sync_detector_get_tick_tolerance(sync_detector_t *sd) {
    return sd ? sd->tick_phase_tolerance_ms : TICK_PHASE_TOLERANCE_MS;
}

void sync_detector_set_marker_tolerance(sync_detector_t *sd, float ms) {
    if (!sd || ms < 200.0f || ms > 800.0f) return;
    sd->marker_tolerance_ms = ms;
}

float sync_detector_get_marker_tolerance(sync_detector_t *sd) {
    return sd ? sd->marker_tolerance_ms : MARKER_TOLERANCE_MS;
}

void sync_detector_set_p_marker_tolerance(sync_detector_t *sd, float ms) {
    if (!sd || ms < 100.0f || ms > 400.0f) return;
    sd->p_marker_tolerance_ms = ms;
}

float sync_detector_get_p_marker_tolerance(sync_detector_t *sd) {
    return sd ? sd->p_marker_tolerance_ms : P_MARKER_TOLERANCE_MS;
}
