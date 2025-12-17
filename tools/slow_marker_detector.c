/**
 * @file slow_marker_detector.c
 * @brief Slow-path marker detector implementation
 */

#include "slow_marker_detector.h"
#include "kiss_fft.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define HZ_PER_BIN      ((float)SLOW_MARKER_SAMPLE_RATE / SLOW_MARKER_FFT_SIZE)  /* 5.86 Hz */
#define FRAME_MS        85.0f   /* Effective frame rate with 50% overlap */

/* Threshold: accumulated energy must be 2x noise (10-frame sum) */
#define SLOW_THRESHOLD_MULT     2.0f
#define NOISE_ADAPT_RATE        0.02f

struct slow_marker_detector {
    /* Accumulator ring buffer */
    float energy_history[SLOW_MARKER_ACCUM_FRAMES];
    int history_idx;
    int history_count;
    float accumulated_energy;

    /* Noise tracking */
    float noise_floor;
    float threshold;

    /* Current state */
    float current_energy;
    float current_snr_db;
    bool above_threshold;
    float timestamp_ms;

    /* Callback */
    slow_marker_callback_fn callback;
    void *callback_user_data;
};

slow_marker_detector_t *slow_marker_detector_create(void) {
    slow_marker_detector_t *smd = calloc(1, sizeof(*smd));
    if (!smd) return NULL;

    smd->noise_floor = 0.01f;
    smd->threshold = smd->noise_floor * SLOW_THRESHOLD_MULT * SLOW_MARKER_ACCUM_FRAMES;

    printf("[SLOW_MARKER] Created: %.1f Hz/bin, %d-frame accumulator (%.0fms)\n",
           HZ_PER_BIN, SLOW_MARKER_ACCUM_FRAMES, SLOW_MARKER_ACCUM_FRAMES * FRAME_MS);

    return smd;
}

void slow_marker_detector_destroy(slow_marker_detector_t *smd) {
    free(smd);
}

void slow_marker_detector_process_fft(slow_marker_detector_t *smd,
                                       const kiss_fft_cpx *fft_out,
                                       float timestamp_ms) {
    if (!smd || !fft_out) return;

    /* Extract tight 1000 Hz bucket energy */
    int center_bin = (int)(SLOW_MARKER_TARGET_HZ / HZ_PER_BIN + 0.5f);  /* bin 170 */
    int bin_span = (int)(SLOW_MARKER_BANDWIDTH_HZ / 2.0f / HZ_PER_BIN + 0.5f);  /* ±8 bins */

    float signal_energy = 0.0f;
    float noise_energy = 0.0f;
    int noise_bins = 0;

    /* Signal bucket: 950-1050 Hz */
    for (int b = -bin_span; b <= bin_span; b++) {
        int bin = center_bin + b;
        if (bin >= 0 && bin < SLOW_MARKER_FFT_SIZE / 2) {
            float re = fft_out[bin].r;
            float im = fft_out[bin].i;
            signal_energy += sqrtf(re * re + im * im) / SLOW_MARKER_FFT_SIZE;
        }
    }

    /* Noise estimate from adjacent buckets (800-900 Hz and 1100-1200 Hz) */
    for (int offset = -3; offset <= -2; offset++) {  /* Below signal */
        int bin = center_bin + offset * bin_span;
        if (bin >= 0 && bin < SLOW_MARKER_FFT_SIZE / 2) {
            float re = fft_out[bin].r;
            float im = fft_out[bin].i;
            noise_energy += sqrtf(re * re + im * im) / SLOW_MARKER_FFT_SIZE;
            noise_bins++;
        }
    }
    for (int offset = 2; offset <= 3; offset++) {  /* Above signal */
        int bin = center_bin + offset * bin_span;
        if (bin >= 0 && bin < SLOW_MARKER_FFT_SIZE / 2) {
            float re = fft_out[bin].r;
            float im = fft_out[bin].i;
            noise_energy += sqrtf(re * re + im * im) / SLOW_MARKER_FFT_SIZE;
            noise_bins++;
        }
    }

    /* Per-frame noise estimate */
    float frame_noise = (noise_bins > 0) ? noise_energy / noise_bins : 0.001f;

    /* Update noise floor (slow adaptation, only when not detecting) */
    if (!smd->above_threshold) {
        smd->noise_floor += NOISE_ADAPT_RATE * (frame_noise - smd->noise_floor);
        if (smd->noise_floor < 0.0001f) smd->noise_floor = 0.0001f;
    }

    /* Update sliding accumulator */
    if (smd->history_count >= SLOW_MARKER_ACCUM_FRAMES) {
        smd->accumulated_energy -= smd->energy_history[smd->history_idx];
    }
    smd->energy_history[smd->history_idx] = signal_energy;
    smd->accumulated_energy += signal_energy;
    smd->history_idx = (smd->history_idx + 1) % SLOW_MARKER_ACCUM_FRAMES;
    if (smd->history_count < SLOW_MARKER_ACCUM_FRAMES) {
        smd->history_count++;
    }

    /* Update threshold and state */
    smd->threshold = smd->noise_floor * SLOW_THRESHOLD_MULT * SLOW_MARKER_ACCUM_FRAMES;
    smd->current_energy = smd->accumulated_energy;
    smd->above_threshold = (smd->accumulated_energy > smd->threshold);
    smd->timestamp_ms = timestamp_ms;

    /* Calculate SNR */
    float noise_sum = smd->noise_floor * SLOW_MARKER_ACCUM_FRAMES;
    smd->current_snr_db = (noise_sum > 0.0001f) ?
        20.0f * log10f(smd->accumulated_energy / noise_sum) : 0.0f;

    /* Callback */
    if (smd->callback) {
        slow_marker_frame_t frame = {
            .energy = smd->accumulated_energy,
            .snr_db = smd->current_snr_db,
            .noise_floor = smd->noise_floor,
            .timestamp_ms = timestamp_ms,
            .above_threshold = smd->above_threshold
        };
        smd->callback(&frame, smd->callback_user_data);
    }
}

void slow_marker_detector_set_callback(slow_marker_detector_t *smd,
                                        slow_marker_callback_fn cb, void *user_data) {
    if (!smd) return;
    smd->callback = cb;
    smd->callback_user_data = user_data;
}

float slow_marker_detector_get_noise_floor(slow_marker_detector_t *smd) {
    return smd ? smd->noise_floor : 0.0f;
}

float slow_marker_detector_get_current_energy(slow_marker_detector_t *smd) {
    return smd ? smd->current_energy : 0.0f;
}

bool slow_marker_detector_is_above_threshold(slow_marker_detector_t *smd) {
    return smd ? smd->above_threshold : false;
}
