/**
 * @file tone_tracker.h
 * @brief WWV tone frequency tracker for receiver characterization
 *
 * Tracks 500 Hz and 600 Hz reference tones to measure receiver
 * LO offset and drift. Uses parabolic interpolation for sub-bin
 * frequency accuracy.
 */

#ifndef TONE_TRACKER_H
#define TONE_TRACKER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct tone_tracker tone_tracker_t;

/*============================================================================
 * Configuration
 *============================================================================*/

#define TONE_SAMPLE_RATE        12000       /* Match display path */
#define TONE_FFT_SIZE           4096        /* 2.93 Hz/bin */
#define TONE_HZ_PER_BIN         ((float)TONE_SAMPLE_RATE / TONE_FFT_SIZE)
#define TONE_FRAME_MS           ((float)TONE_FFT_SIZE * 1000.0f / TONE_SAMPLE_RATE)

#define CARRIER_NOMINAL_HZ      10000000.0f /* 10 MHz WWV for PPM scaling */

/*============================================================================
 * API
 *============================================================================*/

/* Create tracker for specific nominal frequency (500 or 600 Hz) */
tone_tracker_t *tone_tracker_create(float nominal_hz, const char *csv_path);
void tone_tracker_destroy(tone_tracker_t *tt);

/* Feed samples (from 12 kHz display path) */
void tone_tracker_process_sample(tone_tracker_t *tt, float i, float q);

/* Query results */
float tone_tracker_get_measured_hz(tone_tracker_t *tt);
float tone_tracker_get_offset_hz(tone_tracker_t *tt);
float tone_tracker_get_offset_ppm(tone_tracker_t *tt);
float tone_tracker_get_snr_db(tone_tracker_t *tt);
float tone_tracker_get_noise_floor(tone_tracker_t *tt);
bool tone_tracker_is_valid(tone_tracker_t *tt);
uint64_t tone_tracker_get_frame_count(tone_tracker_t *tt);

/* Global subcarrier noise floor - exported for marker detector baseline */
extern float g_subcarrier_noise_floor;

/* Update global noise floor from active subcarrier tracker */
void tone_tracker_update_global_noise_floor(tone_tracker_t *tt);

#endif /* TONE_TRACKER_H */
