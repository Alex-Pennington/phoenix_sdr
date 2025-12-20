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

#define TICK_FFT_SIZE           256     /* 5.12ms frames at 50kHz - matches 5ms WWV pulse */
#define TICK_SAMPLE_RATE        50000   /* Expected input sample rate (2MHz/40 = exact) */
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
    float corr_peak;
    float corr_ratio;
} tick_event_t;

typedef void (*tick_callback_fn)(const tick_event_t *event, void *user_data);

/*============================================================================
 * Callback for minute marker events (detected by duration)
 *============================================================================*/

/* Timing calibration constants (WWV spec + empirical measurements) */
#define TICK_ACTUAL_DURATION_MS    5.0f     /* WWV spec: 5ms pulse */
#define MARKER_ACTUAL_DURATION_MS  800.0f   /* WWV spec: 800ms pulse */
#define TICK_FILTER_DELAY_MS       3.0f     /* Filter group delay (2.55ms Hann + 0.32ms Butterworth + 0.13ms decimation) */

typedef struct {
    int marker_number;
    float timestamp_ms;        /* TRAILING EDGE - when pulse energy dropped below threshold */
    float start_timestamp_ms;  /* LEADING EDGE - timestamp_ms - duration_ms - TICK_FILTER_DELAY_MS (ON-TIME MARKER) */
    float duration_ms;         /* Measured duration (may be biased longer than actual due to threshold hysteresis) */
    float corr_ratio;
    float interval_ms;         /* Time since previous marker */
} tick_marker_event_t;

typedef void (*tick_marker_callback_fn)(const tick_marker_event_t *event, void *user_data);

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
 * Set callback for minute marker events (duration-based detection)
 * @param td        Detector instance
 * @param callback  Function to call when minute marker detected
 * @param user_data Passed to callback
 */
void tick_detector_set_marker_callback(tick_detector_t *td, tick_marker_callback_fn callback, void *user_data);

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
 * Log a metadata change to CSV (gain, frequency, etc.)
 * @param td              Detector instance
 * @param center_freq     Center frequency in Hz
 * @param sample_rate     Sample rate in Hz
 * @param gain_reduction  IF gain reduction in dB
 * @param lna_state       LNA state (0-8)
 */
void tick_detector_log_metadata(tick_detector_t *td, uint64_t center_freq,
                                uint32_t sample_rate, uint32_t gain_reduction,
                                uint32_t lna_state);

/**
 * Log a display gain change to CSV
 * @param td              Detector instance
 * @param display_gain_db Display gain offset in dB
 */
void tick_detector_log_display_gain(tick_detector_t *td, float display_gain_db);

/**
 * Get timing info for display
 */
float tick_detector_get_frame_duration_ms(void);

/**
 * Epoch source - tracks where epoch timing came from
 */
typedef enum {
    EPOCH_SOURCE_NONE = 0,        /* No epoch set yet */
    EPOCH_SOURCE_MARKER = 1,      /* From marker detector (±50ms precision) */
    EPOCH_SOURCE_TICK_CHAIN = 2   /* From tick correlation chain (±5ms precision) */
} epoch_source_t;

/**
 * Timing gate control (for marker-based epoch bootstrap)
 * Set epoch from marker timestamp: epoch_ms = marker_timestamp - 10.0
 */
void tick_detector_set_epoch(tick_detector_t *td, float epoch_ms);

/**
 * Set timing gate epoch with source and confidence tracking
 * @param source Where epoch came from (marker or tick chain)
 * @param confidence Confidence metric 0-1 (higher = more precise)
 */
void tick_detector_set_epoch_with_source(tick_detector_t *td, float epoch_ms,
                                          epoch_source_t source, float confidence);

void tick_detector_set_gating_enabled(tick_detector_t *td, bool enabled);
float tick_detector_get_epoch(tick_detector_t *td);
bool tick_detector_is_gating_enabled(tick_detector_t *td);

/**
 * Get current epoch source and confidence
 */
epoch_source_t tick_detector_get_epoch_source(tick_detector_t *td);
float tick_detector_get_epoch_confidence(tick_detector_t *td);

/**
 * Runtime tunable parameters (UDP command interface)
 * Ranges validated in setters, invalid values rejected with false return
 */
bool tick_detector_set_threshold_mult(tick_detector_t *td, float value);    /* 1.0-5.0 */
bool tick_detector_set_adapt_alpha_down(tick_detector_t *td, float value);  /* 0.9-0.999 */
bool tick_detector_set_adapt_alpha_up(tick_detector_t *td, float value);    /* 0.001-0.1 */
bool tick_detector_set_min_duration_ms(tick_detector_t *td, float value);   /* 1.0-10.0 */

float tick_detector_get_threshold_mult(tick_detector_t *td);
float tick_detector_get_adapt_alpha_down(tick_detector_t *td);
float tick_detector_get_adapt_alpha_up(tick_detector_t *td);
float tick_detector_get_min_duration_ms(tick_detector_t *td);

#ifdef __cplusplus
}
#endif

#endif /* TICK_DETECTOR_H */
