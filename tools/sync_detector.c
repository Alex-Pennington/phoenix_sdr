/**
 * @file sync_detector.c
 * @brief WWV sync detector implementation
 *
 * Correlates inputs from tick detector and marker detector to confirm
 * minute markers with high confidence. Tracks sync state and outputs
 * confirmed markers to CSV log.
 */

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

#define CORRELATION_WINDOW_MS   1500.0f  /* Max time between tick/marker detections to correlate */
#define MARKER_INTERVAL_MIN_MS  55000.0f /* Minimum valid interval between markers */
#define MARKER_INTERVAL_MAX_MS  65000.0f /* Maximum valid interval between markers */
#define MARKER_NOMINAL_MS       60000.0f /* Nominal marker interval */
#define PENDING_TIMEOUT_MS      3000.0f  /* Clear pending event if not correlated in time */
#define SYNC_FLASH_FRAMES       30       /* Flash duration for confirmed marker */

/*============================================================================
 * Internal State
 *============================================================================*/

struct sync_detector {
    /* Pending event from tick detector */
    float pending_tick_ms;
    float pending_tick_duration_ms;
    float pending_tick_corr_ratio;
    bool tick_pending;

    /* Pending event from marker detector */
    float pending_marker_ms;
    float pending_marker_energy;
    float pending_marker_duration_ms;
    bool marker_pending;

    /* Confirmed markers */
    float last_confirmed_ms;
    float prev_confirmed_ms;
    int confirmed_count;
    int good_intervals;          /* Count of ~60s intervals */

    /* State */
    sync_state_t state;

    /* UI feedback */
    int flash_frames_remaining;

    /* Logging */
    FILE *csv_file;
    time_t start_time;
};

/*============================================================================
 * Internal Functions
 *============================================================================*/

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

    /* UDP telemetry broadcast */
    telem_sendf(TELEM_SYNC, "%s,%.1f,%d,%s,%.1f,%.0f,%.1f,%.1f",
                time_str, timestamp_ms, sd->confirmed_count,
                sync_state_name(sd->state), interval_ms / 1000.0f,
                delta_ms, sd->pending_tick_duration_ms,
                sd->pending_marker_duration_ms);
}

static void try_correlate(sync_detector_t *sd, float current_ms) {
    /* Need both events pending */
    if (!sd->tick_pending || !sd->marker_pending)
        return;

    float delta = fabsf(sd->pending_marker_ms - sd->pending_tick_ms);

    if (delta < CORRELATION_WINDOW_MS) {
        /* CONFIRMED - both detectors agree! */
        /* Use tick detector's timestamp (more precise, fires at pulse end) */
        float marker_time = sd->pending_tick_ms;

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
            sd->confirmed_count++;

            /* Track good intervals for lock confidence */
            if (interval_ms >= MARKER_INTERVAL_MIN_MS &&
                interval_ms <= MARKER_INTERVAL_MAX_MS) {
                sd->good_intervals++;
            }

            /* Update state */
            if (sd->good_intervals >= 2) {
                sd->state = SYNC_LOCKED;
            } else if (sd->confirmed_count >= 1) {
                sd->state = SYNC_TENTATIVE;
            }

            /* Flash for UI */
            sd->flash_frames_remaining = SYNC_FLASH_FRAMES;

            /* Log to CSV */
            log_confirmed_marker(sd, marker_time, interval_ms, delta, "CORRELATED");

            /* Console output */
            printf("[SYNC] *** CONFIRMED MARKER #%d *** delta=%.0fms interval=%.1fs state=%s\n",
                   sd->confirmed_count, delta, interval_ms / 1000.0f,
                   sync_state_name(sd->state));
        } else {
            printf("[SYNC] Correlated marker rejected: interval=%.1fs (out of range)\n",
                   interval_ms / 1000.0f);
        }

        /* Clear both pending events */
        sd->tick_pending = false;
        sd->marker_pending = false;
    }
}

static void check_timeout(sync_detector_t *sd, float current_ms) {
    /* Clear stale pending events that weren't correlated */
    if (sd->tick_pending && (current_ms - sd->pending_tick_ms) > PENDING_TIMEOUT_MS) {
        printf("[SYNC] Tick marker timed out (no correlation)\n");
        sd->tick_pending = false;
    }
    if (sd->marker_pending && (current_ms - sd->pending_marker_ms) > PENDING_TIMEOUT_MS) {
        printf("[SYNC] Marker event timed out (no correlation)\n");
        sd->marker_pending = false;
    }
}

/*============================================================================
 * Public API
 *============================================================================*/

sync_detector_t *sync_detector_create(const char *csv_path) {
    sync_detector_t *sd = (sync_detector_t *)calloc(1, sizeof(sync_detector_t));
    if (!sd) return NULL;

    sd->state = SYNC_ACQUIRING;
    sd->start_time = time(NULL);

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
        case SYNC_ACQUIRING: return "ACQUIRING";
        case SYNC_TENTATIVE: return "TENTATIVE";
        case SYNC_LOCKED:    return "LOCKED";
        default:             return "UNKNOWN";
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
