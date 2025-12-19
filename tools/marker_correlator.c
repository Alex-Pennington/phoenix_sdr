/**
 * @file marker_correlator.c
 * @brief Correlates fast/slow marker detections
 */

#include "marker_correlator.h"
#include "waterfall_telemetry.h"
#include "version.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Correlation window increased from 500ms to 750ms. The slow path (12kHz) updates
 * every ~85ms, so 500ms only allowed ~6 slow frames to catch correlation. With
 * path timing differences, edge cases could miss. 750ms provides ~9 slow frames,
 * more margin for timing jitter. Still well under 30-second marker cooldown so
 * no risk of false correlations. Added v1.0.1+19, 2025-12-17. */
#define CORRELATION_WINDOW_MS   750.0f  /* Fast/slow must agree within 750ms */
#define MIN_DURATION_MS         500.0f  /* Minimum duration for fast path */

struct marker_correlator {
    /* Pending fast detection (waiting for slow confirmation) */
    bool fast_pending;
    float fast_timestamp_ms;
    float fast_duration_ms;

    /* Slow path state during fast event window */
    bool slow_triggered;
    float slow_peak_energy;
    float slow_peak_snr;

    /* Statistics */
    int markers_confirmed;
    int markers_fast_only;
    int markers_slow_only;

    /* Callback */
    correlated_marker_callback_fn callback;
    void *callback_user_data;

    /* Logging */
    FILE *csv_file;
    time_t start_time;
};

marker_correlator_t *marker_correlator_create(const char *csv_path) {
    marker_correlator_t *mc = calloc(1, sizeof(*mc));
    if (!mc) return NULL;

    mc->start_time = time(NULL);

    if (csv_path) {
        mc->csv_file = fopen(csv_path, "w");
        if (mc->csv_file) {
            char time_str[64];
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&mc->start_time));
            fprintf(mc->csv_file, "# Phoenix SDR Correlated Marker Log v%s\n", PHOENIX_VERSION_FULL);
            fprintf(mc->csv_file, "# Started: %s\n", time_str);
            fprintf(mc->csv_file, "time,timestamp_ms,marker_num,duration_ms,energy,snr_db,confidence\n");
            fflush(mc->csv_file);
        }
    }

    printf("[CORRELATOR] Created: window=%.0fms, min_dur=%.0fms\n",
           CORRELATION_WINDOW_MS, MIN_DURATION_MS);

    return mc;
}

void marker_correlator_destroy(marker_correlator_t *mc) {
    if (!mc) return;

    printf("[CORRELATOR] Stats: confirmed=%d, fast_only=%d, slow_only=%d\n",
           mc->markers_confirmed, mc->markers_fast_only, mc->markers_slow_only);

    if (mc->csv_file) fclose(mc->csv_file);
    free(mc);
}

void marker_correlator_fast_event(marker_correlator_t *mc,
                                   float timestamp_ms,
                                   float duration_ms) {
    if (!mc) return;

    /* Store fast detection, wait for slow confirmation */
    mc->fast_pending = true;
    mc->fast_timestamp_ms = timestamp_ms;
    mc->fast_duration_ms = duration_ms;
    mc->slow_triggered = false;
    mc->slow_peak_energy = 0.0f;
    mc->slow_peak_snr = 0.0f;
}

void marker_correlator_slow_frame(marker_correlator_t *mc,
                                   float timestamp_ms,
                                   float energy,
                                   float snr_db,
                                   bool above_threshold) {
    if (!mc) return;

    /* Track slow path state */
    if (above_threshold) {
        mc->slow_triggered = true;
        if (energy > mc->slow_peak_energy) {
            mc->slow_peak_energy = energy;
            mc->slow_peak_snr = snr_db;
        }
    }

    /* Check for correlation if fast detection pending */
    if (mc->fast_pending) {
        float elapsed = timestamp_ms - mc->fast_timestamp_ms;

        /* Within correlation window? */
        if (elapsed > CORRELATION_WINDOW_MS) {
            /* Window expired - emit result */
            marker_confidence_t conf;

            if (mc->fast_duration_ms >= MIN_DURATION_MS && mc->slow_triggered) {
                conf = MARKER_CONF_HIGH;
                mc->markers_confirmed++;
            } else if (mc->fast_duration_ms >= MIN_DURATION_MS) {
                conf = MARKER_CONF_LOW;
                mc->markers_fast_only++;
            } else if (mc->slow_triggered) {
                conf = MARKER_CONF_LOW;
                mc->markers_slow_only++;
            } else {
                conf = MARKER_CONF_NONE;
            }

            if (conf != MARKER_CONF_NONE) {
                int marker_num = mc->markers_confirmed + mc->markers_fast_only + mc->markers_slow_only;
                const char *conf_str = (conf == MARKER_CONF_HIGH) ? "HIGH" : "LOW";

                printf("[CORRELATOR] MARKER #%d  dur=%.0fms  energy=%.1f  snr=%.1fdB  conf=%s\n",
                       marker_num, mc->fast_duration_ms, mc->slow_peak_energy,
                       mc->slow_peak_snr, conf_str);

                time_t event_time = mc->start_time + (time_t)(mc->fast_timestamp_ms / 1000.0f);
                struct tm *tm_info = localtime(&event_time);
                char time_str[16];
                strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);

                if (mc->csv_file) {
                    fprintf(mc->csv_file, "%s,%.1f,%d,%.1f,%.4f,%.1f,%s\n",
                            time_str, mc->fast_timestamp_ms, marker_num,
                            mc->fast_duration_ms, mc->slow_peak_energy,
                            mc->slow_peak_snr, conf_str);
                    fflush(mc->csv_file);
                }

                /* UDP telemetry */
                telem_sendf(TELEM_MARKERS, "%s,%.1f,%d,%.1f,%.4f,%.1f,%s",
                            time_str, mc->fast_timestamp_ms, marker_num,
                            mc->fast_duration_ms, mc->slow_peak_energy,
                            mc->slow_peak_snr, conf_str);

                if (mc->callback) {
                    correlated_marker_t marker = {
                        .marker_number = marker_num,
                        .timestamp_ms = mc->fast_timestamp_ms,
                        .duration_ms = mc->fast_duration_ms,
                        .energy = mc->slow_peak_energy,
                        .snr_db = mc->slow_peak_snr,
                        .confidence = conf
                    };
                    mc->callback(&marker, mc->callback_user_data);
                }
            }

            mc->fast_pending = false;
        }
    }
}

void marker_correlator_set_callback(marker_correlator_t *mc,
                                     correlated_marker_callback_fn cb, void *user_data) {
    if (!mc) return;
    mc->callback = cb;
    mc->callback_user_data = user_data;
}
