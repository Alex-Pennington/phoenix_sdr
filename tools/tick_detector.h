/**
 * @file tick_detector.h
 * @brief WWV tick pulse detector module
 *
 * Self-contained detector with optimized FFT for time-domain precision.
 * Designed as a template for additional detection channels.
 *
 * Pattern for replication:
 *   - Each detector has its own FFT sized for its signal
 *   - Each detector maintains its own sample buffer
 *   - Each detector runs its own state machine
 *   - Detectors can run in parallel on same sample stream
 */

#ifndef TICK_DETECTOR_H
#define TICK_DETECTOR_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Configuration - Tune these for the target signal
 *============================================================================*/

#define TICK_FFT_SIZE           256     /* 5.3ms frames at 48kHz - matches 5ms WWV pulse */
#define TICK_SAMPLE_RATE        48000   /* Expected input sample rate */
#define TICK_TARGET_FREQ_HZ     1000    /* Frequency bucket to watch */
#define TICK_BANDWIDTH_HZ       100     /* Width of detection bucket */

/* Matched filter template */
#define TICK_PULSE_MS           5.0f    /* WWV tick pulse duration */
#define TICK_TEMPLATE_SAMPLES   ((int)(TICK_PULSE_MS * TICK_SAMPLE_RATE / 1000.0f))  /* 240 samples */
#define TICK_CORR_BUFFER_SIZE   512     /* Must be > TICK_TEMPLATE_SAMPLES */

/*============================================================================
 * Detector State (opaque to caller)
 *============================================================================*/

typedef struct tick_detector tick_detector_t;

/*============================================================================
 * Callback for tick events
 *============================================================================*/

typedef struct {
    int tick_number;
    float timestamp_ms;
    float interval_ms;
    float duration_ms;
    float peak_energy;
    float avg_interval_ms;
    float noise_floor;
} tick_event_t;

typedef void (*tick_callback_fn)(const tick_event_t *event, void *user_data);

/*============================================================================
 * Public API
 *============================================================================*/

/**
 * Create a new tick detector instance
 * @param csv_path  Path for CSV log file (NULL to disable logging)
 * @return          Detector instance or NULL on failure
 */
tick_detector_t *tick_detector_create(const char *csv_path);

/**
 * Destroy a tick detector instance
 */
void tick_detector_destroy(tick_detector_t *td);

/**
 * Set callback for tick events
 * @param td        Detector instance
 * @param callback  Function to call when tick detected
 * @param user_data Passed to callback
 */
void tick_detector_set_callback(tick_detector_t *td, tick_callback_fn callback, void *user_data);

/**
 * Feed I/Q samples to detector
 * Detector buffers internally and runs FFT when ready
 * @param td        Detector instance
 * @param i_sample  In-phase sample
 * @param q_sample  Quadrature sample
 * @return          true if a tick was detected this sample
 */
bool tick_detector_process_sample(tick_detector_t *td, float i_sample, float q_sample);

/**
 * Check if detector is flashing (for UI display)
 * @param td        Detector instance
 * @return          Frames remaining in flash, 0 if not flashing
 */
int tick_detector_get_flash_frames(tick_detector_t *td);

/**
 * Decrement flash counter (call once per display frame)
 */
void tick_detector_decrement_flash(tick_detector_t *td);

/**
 * Enable/disable detection
 */
void tick_detector_set_enabled(tick_detector_t *td, bool enabled);
bool tick_detector_get_enabled(tick_detector_t *td);

/**
 * Get current state for display
 */
float tick_detector_get_noise_floor(tick_detector_t *td);
float tick_detector_get_threshold(tick_detector_t *td);
float tick_detector_get_current_energy(tick_detector_t *td);
int tick_detector_get_tick_count(tick_detector_t *td);

/**
 * Print statistics to stdout
 */
void tick_detector_print_stats(tick_detector_t *td);

/**
 * Get timing info for display
 */
float tick_detector_get_frame_duration_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* TICK_DETECTOR_H */
