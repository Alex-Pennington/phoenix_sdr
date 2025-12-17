/**
 * @file wwv_detector_manager.c
 * @brief Centralized WWV detector orchestration implementation
 *
 * See wwv_detector_manager.h for architecture documentation.
 */

#include "wwv_detector_manager.h"
#include "tick_detector.h"
#include "marker_detector.h"
#include "sync_detector.h"
#include "tone_tracker.h"
#include "tick_correlator.h"
#include "marker_correlator.h"
#include "slow_marker_detector.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/*============================================================================
 * Internal State
 *============================================================================*/

struct wwv_detector_manager {
    /* Detector path (50 kHz) */
    tick_detector_t *tick_detector;
    marker_detector_t *marker_detector;
    
    /* Correlators */
    tick_correlator_t *tick_correlator;
    marker_correlator_t *marker_correlator;
    sync_detector_t *sync_detector;
    
    /* Display path (12 kHz) */
    tone_tracker_t *tone_carrier;
    tone_tracker_t *tone_500;
    tone_tracker_t *tone_600;
    slow_marker_detector_t *slow_marker;
    
    /* External callbacks */
    wwv_tick_callback_fn tick_callback;
    void *tick_callback_data;
    wwv_marker_callback_fn marker_callback;
    void *marker_callback_data;
    wwv_sync_callback_fn sync_callback;
    void *sync_callback_data;
    
    /* Statistics */
    uint64_t detector_samples;
    uint64_t display_samples;
};

/*============================================================================
 * Internal Callbacks - Route detector events
 *============================================================================*/

static void on_tick_event(const tick_event_t *event, void *user_data) {
    wwv_detector_manager_t *mgr = (wwv_detector_manager_t *)user_data;
    
    /* Feed correlator */
    if (mgr->tick_correlator) {
        tick_correlator_add_tick(mgr->tick_correlator, 
                                  event->timestamp_ms,
                                  event->duration_ms);
    }
    
    /* Forward to external callback */
    if (mgr->tick_callback) {
        wwv_tick_event_t ext_event = {
            .tick_number = event->tick_number,
            .timestamp_ms = event->timestamp_ms,
            .duration_ms = event->duration_ms,
            .energy = event->peak_energy
        };
        mgr->tick_callback(&ext_event, mgr->tick_callback_data);
    }
}

static void on_tick_marker_event(const tick_marker_event_t *event, void *user_data) {
    wwv_detector_manager_t *mgr = (wwv_detector_manager_t *)user_data;
    
    /* Feed sync detector */
    if (mgr->sync_detector) {
        sync_detector_tick_marker(mgr->sync_detector,
                                   event->timestamp_ms,
                                   event->duration_ms,
                                   event->corr_ratio);
    }
}

static void on_marker_event(const marker_event_t *event, void *user_data) {
    wwv_detector_manager_t *mgr = (wwv_detector_manager_t *)user_data;
    
    /* Feed correlator */
    if (mgr->marker_correlator) {
        marker_correlator_fast_event(mgr->marker_correlator,
                                      event->timestamp_ms,
                                      event->duration_ms);
    }
    
    /* Forward to external callback */
    if (mgr->marker_callback) {
        wwv_marker_event_t ext_event = {
            .marker_number = event->marker_number,
            .timestamp_ms = event->timestamp_ms,
            .since_last_sec = event->since_last_marker_sec,
            .duration_ms = event->duration_ms,
            .energy = event->accumulated_energy
        };
        mgr->marker_callback(&ext_event, mgr->marker_callback_data);
    }
}

static void on_slow_marker_frame(const slow_marker_frame_t *frame, void *user_data) {
    wwv_detector_manager_t *mgr = (wwv_detector_manager_t *)user_data;
    
    /* Feed correlator for verification */
    if (mgr->marker_correlator) {
        marker_correlator_slow_frame(mgr->marker_correlator,
                                      frame->timestamp_ms,
                                      frame->energy,
                                      frame->snr_db,
                                      frame->above_threshold);
    }
    
    /* NOTE: We do NOT inject slow_marker's baseline into marker_detector!
     * The FFT configurations are incompatible (12kHz/2048 vs 50kHz/256).
     * Each detector tracks its own baseline independently.
     */
}

/*============================================================================
 * Lifecycle
 *============================================================================*/

wwv_detector_manager_t *wwv_detector_manager_create(const wwv_detector_config_t *config) {
    wwv_detector_manager_t *mgr = calloc(1, sizeof(*mgr));
    if (!mgr) return NULL;
    
    char path[512];
    
    printf("\n[DETECTOR_MGR] Creating WWV detector manager...\n");
    
    /* Detector path components */
    if (config->enable_tick_detector) {
        snprintf(path, sizeof(path), "%s/wwv_ticks.csv", config->output_dir);
        mgr->tick_detector = tick_detector_create(path);
        if (mgr->tick_detector) {
            tick_detector_set_callback(mgr->tick_detector, on_tick_event, mgr);
            tick_detector_set_marker_callback(mgr->tick_detector, on_tick_marker_event, mgr);
        }
    }
    
    if (config->enable_marker_detector) {
        snprintf(path, sizeof(path), "%s/wwv_markers.csv", config->output_dir);
        mgr->marker_detector = marker_detector_create(path);
        if (mgr->marker_detector) {
            marker_detector_set_callback(mgr->marker_detector, on_marker_event, mgr);
        }
    }
    
    /* Correlators */
    if (config->enable_correlators) {
        snprintf(path, sizeof(path), "%s/wwv_tick_corr.csv", config->output_dir);
        mgr->tick_correlator = tick_correlator_create(path);
        
        snprintf(path, sizeof(path), "%s/wwv_markers_corr.csv", config->output_dir);
        mgr->marker_correlator = marker_correlator_create(path);
    }
    
    if (config->enable_sync_detector) {
        snprintf(path, sizeof(path), "%s/wwv_sync.csv", config->output_dir);
        mgr->sync_detector = sync_detector_create(path);
    }
    
    /* Display path components */
    if (config->enable_tone_trackers) {
        snprintf(path, sizeof(path), "%s/wwv_carrier.csv", config->output_dir);
        mgr->tone_carrier = tone_tracker_create(0.0f, path);
        
        snprintf(path, sizeof(path), "%s/wwv_tone_500.csv", config->output_dir);
        mgr->tone_500 = tone_tracker_create(500.0f, path);
        
        snprintf(path, sizeof(path), "%s/wwv_tone_600.csv", config->output_dir);
        mgr->tone_600 = tone_tracker_create(600.0f, path);
    }
    
    if (config->enable_slow_marker) {
        mgr->slow_marker = slow_marker_detector_create();
        if (mgr->slow_marker) {
            slow_marker_detector_set_callback(mgr->slow_marker, on_slow_marker_frame, mgr);
        }
    }
    
    printf("[DETECTOR_MGR] Created: tick=%s marker=%s sync=%s tones=%s slow=%s\n",
           mgr->tick_detector ? "YES" : "no",
           mgr->marker_detector ? "YES" : "no",
           mgr->sync_detector ? "YES" : "no",
           mgr->tone_carrier ? "YES" : "no",
           mgr->slow_marker ? "YES" : "no");
    
    return mgr;
}

void wwv_detector_manager_destroy(wwv_detector_manager_t *mgr) {
    if (!mgr) return;
    
    printf("[DETECTOR_MGR] Destroying...\n");
    
    /* Print final stats */
    wwv_detector_manager_print_stats(mgr);
    
    /* Destroy in reverse order */
    if (mgr->slow_marker) slow_marker_detector_destroy(mgr->slow_marker);
    if (mgr->tone_600) tone_tracker_destroy(mgr->tone_600);
    if (mgr->tone_500) tone_tracker_destroy(mgr->tone_500);
    if (mgr->tone_carrier) tone_tracker_destroy(mgr->tone_carrier);
    if (mgr->sync_detector) sync_detector_destroy(mgr->sync_detector);
    if (mgr->marker_correlator) marker_correlator_destroy(mgr->marker_correlator);
    if (mgr->tick_correlator) tick_correlator_destroy(mgr->tick_correlator);
    if (mgr->marker_detector) marker_detector_destroy(mgr->marker_detector);
    if (mgr->tick_detector) tick_detector_destroy(mgr->tick_detector);
    
    free(mgr);
}

/*============================================================================
 * Sample Processing
 *============================================================================*/

void wwv_detector_manager_process_detector_sample(wwv_detector_manager_t *mgr,
                                                   float i_sample, float q_sample) {
    if (!mgr) return;
    
    if (mgr->tick_detector) {
        tick_detector_process_sample(mgr->tick_detector, i_sample, q_sample);
    }
    
    if (mgr->marker_detector) {
        marker_detector_process_sample(mgr->marker_detector, i_sample, q_sample);
    }
    
    mgr->detector_samples++;
}

void wwv_detector_manager_process_display_sample(wwv_detector_manager_t *mgr,
                                                  float i_sample, float q_sample) {
    if (!mgr) return;
    
    if (mgr->tone_carrier) {
        tone_tracker_process_sample(mgr->tone_carrier, i_sample, q_sample);
    }
    if (mgr->tone_500) {
        tone_tracker_process_sample(mgr->tone_500, i_sample, q_sample);
    }
    if (mgr->tone_600) {
        tone_tracker_process_sample(mgr->tone_600, i_sample, q_sample);
    }
    
    mgr->display_samples++;
}

void wwv_detector_manager_process_display_fft(wwv_detector_manager_t *mgr,
                                               const kiss_fft_cpx *fft_out,
                                               float timestamp_ms) {
    if (!mgr || !fft_out) return;
    
    if (mgr->slow_marker) {
        slow_marker_detector_process_fft(mgr->slow_marker, fft_out, timestamp_ms);
    }
}

/*============================================================================
 * Callbacks
 *============================================================================*/

void wwv_detector_manager_set_tick_callback(wwv_detector_manager_t *mgr,
                                             wwv_tick_callback_fn cb, void *user_data) {
    if (!mgr) return;
    mgr->tick_callback = cb;
    mgr->tick_callback_data = user_data;
}

void wwv_detector_manager_set_marker_callback(wwv_detector_manager_t *mgr,
                                               wwv_marker_callback_fn cb, void *user_data) {
    if (!mgr) return;
    mgr->marker_callback = cb;
    mgr->marker_callback_data = user_data;
}

void wwv_detector_manager_set_sync_callback(wwv_detector_manager_t *mgr,
                                             wwv_sync_callback_fn cb, void *user_data) {
    if (!mgr) return;
    mgr->sync_callback = cb;
    mgr->sync_callback_data = user_data;
}

/*============================================================================
 * Status / Diagnostics
 *============================================================================*/

wwv_sync_status_t wwv_detector_manager_get_sync_status(wwv_detector_manager_t *mgr) {
    wwv_sync_status_t status = {0};
    
    if (mgr && mgr->sync_detector) {
        status.is_synced = sync_detector_is_synced(mgr->sync_detector);
        status.confidence = sync_detector_get_confidence(mgr->sync_detector);
        status.drift_ppm = sync_detector_get_drift(mgr->sync_detector);
    }
    
    if (mgr) {
        status.tick_count = wwv_detector_manager_get_tick_count(mgr);
        status.marker_count = wwv_detector_manager_get_marker_count(mgr);
    }
    
    return status;
}

int wwv_detector_manager_get_tick_count(wwv_detector_manager_t *mgr) {
    return (mgr && mgr->tick_detector) ? tick_detector_get_tick_count(mgr->tick_detector) : 0;
}

int wwv_detector_manager_get_marker_count(wwv_detector_manager_t *mgr) {
    return (mgr && mgr->marker_detector) ? marker_detector_get_marker_count(mgr->marker_detector) : 0;
}

int wwv_detector_manager_get_tick_flash(wwv_detector_manager_t *mgr) {
    return (mgr && mgr->tick_detector) ? tick_detector_get_flash_frames(mgr->tick_detector) : 0;
}

int wwv_detector_manager_get_marker_flash(wwv_detector_manager_t *mgr) {
    return (mgr && mgr->marker_detector) ? marker_detector_get_flash_frames(mgr->marker_detector) : 0;
}

void wwv_detector_manager_decrement_flash(wwv_detector_manager_t *mgr) {
    if (!mgr) return;
    if (mgr->tick_detector) tick_detector_decrement_flash(mgr->tick_detector);
    if (mgr->marker_detector) marker_detector_decrement_flash(mgr->marker_detector);
}

void wwv_detector_manager_log_metadata(wwv_detector_manager_t *mgr,
                                        uint64_t center_freq,
                                        uint32_t sample_rate,
                                        uint32_t gain_reduction,
                                        uint32_t lna_state) {
    if (!mgr) return;
    
    if (mgr->marker_detector) {
        marker_detector_log_metadata(mgr->marker_detector, center_freq,
                                      sample_rate, gain_reduction, lna_state);
    }
}

void wwv_detector_manager_log_display_gain(wwv_detector_manager_t *mgr, float display_gain) {
    if (!mgr) return;
    
    if (mgr->marker_detector) {
        marker_detector_log_display_gain(mgr->marker_detector, display_gain);
    }
}

void wwv_detector_manager_print_stats(wwv_detector_manager_t *mgr) {
    if (!mgr) return;
    
    printf("\n");
    printf("================================================================================\n");
    printf("                        WWV DETECTOR MANAGER STATS\n");
    printf("================================================================================\n");
    printf("Samples processed: detector=%llu display=%llu\n",
           (unsigned long long)mgr->detector_samples,
           (unsigned long long)mgr->display_samples);
    printf("\n");
    
    if (mgr->tick_detector) {
        tick_detector_print_stats(mgr->tick_detector);
    }
    
    if (mgr->marker_detector) {
        marker_detector_print_stats(mgr->marker_detector);
    }
    
    if (mgr->sync_detector) {
        sync_detector_print_stats(mgr->sync_detector);
    }
    
    printf("================================================================================\n");
}
