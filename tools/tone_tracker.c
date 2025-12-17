/**
 * @file tone_tracker.c
 * @brief WWV tone frequency tracker implementation
 *
 * Measures exact frequency of 500/600 Hz reference tones using:
 * - Both sidebands (USB + LSB) for accuracy
 * - Parabolic interpolation for sub-bin resolution
 * - SNR gating for validity
 */

#include "tone_tracker.h"
#include "kiss_fft.h"
#include "version.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

/*============================================================================
 * Configuration
 *============================================================================*/

#define SEARCH_BINS         10          /* ±10 bins = ±29 Hz search window */
#define MIN_SNR_DB          10.0f       /* Minimum SNR for valid measurement */
#define NOISE_BINS          20          /* Bins to average for noise floor */

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/*============================================================================
 * Internal State
 *============================================================================*/

struct tone_tracker {
    float nominal_hz;           /* 500 or 600 */

    /* Sample buffer */
    float *buffer_i;
    float *buffer_q;
    int buffer_idx;
    int samples_collected;

    /* FFT */
    kiss_fft_cfg fft_cfg;
    kiss_fft_cpx *fft_in;
    kiss_fft_cpx *fft_out;
    float *window;
    float *magnitudes;

    /* Results */
    float measured_hz;
    float offset_hz;
    float offset_ppm;
    float snr_db;
    float noise_floor_linear;   /* Linear noise floor for marker baseline */
    bool valid;

    /* Logging */
    FILE *csv_file;
    uint64_t frame_count;
    time_t start_time;
};

/*============================================================================
 * Window Function
 *============================================================================*/

static void generate_blackman_harris(float *window, int size) {
    const float a0 = 0.35875f;
    const float a1 = 0.48829f;
    const float a2 = 0.14128f;
    const float a3 = 0.01168f;

    for (int i = 0; i < size; i++) {
        float n = (float)i / (float)(size - 1);
        window[i] = a0
                  - a1 * cosf(2.0f * M_PI * n)
                  + a2 * cosf(4.0f * M_PI * n)
                  - a3 * cosf(6.0f * M_PI * n);
    }
}

/*============================================================================
 * Parabolic Interpolation
 *============================================================================*/

static float parabolic_peak(float *mag, int peak_bin, int fft_size) {
    if (peak_bin <= 0 || peak_bin >= fft_size - 1)
        return (float)peak_bin;

    float alpha = mag[peak_bin - 1];
    float beta  = mag[peak_bin];
    float gamma = mag[peak_bin + 1];

    float denom = alpha - 2.0f * beta + gamma;
    if (fabsf(denom) < 1e-10f)
        return (float)peak_bin;

    float p = 0.5f * (alpha - gamma) / denom;
    return (float)peak_bin + p;
}

/*============================================================================
 * Find Peak in Range
 *============================================================================*/

static int find_peak_bin(float *mag, int start, int end, int fft_size) {
    if (start < 0) start = 0;
    if (end >= fft_size) end = fft_size - 1;

    int peak_bin = start;
    float peak_val = mag[start];

    for (int i = start + 1; i <= end; i++) {
        if (mag[i] > peak_val) {
            peak_val = mag[i];
            peak_bin = i;
        }
    }

    return peak_bin;
}

/*============================================================================
 * Estimate Noise Floor
 *============================================================================*/

static float estimate_noise_floor(float *mag, int fft_size, int exclude_bin, int exclude_range) {
    float sum = 0.0f;
    int count = 0;

    /* Sample bins away from the tone */
    for (int i = 50; i < 150; i++) {
        if (abs(i - exclude_bin) > exclude_range) {
            sum += mag[i];
            count++;
        }
    }

    /* Also check negative frequency region */
    int neg_exclude = fft_size - exclude_bin;
    for (int i = fft_size - 150; i < fft_size - 50; i++) {
        if (abs(i - neg_exclude) > exclude_range) {
            sum += mag[i];
            count++;
        }
    }

    return (count > 0) ? (sum / count) : 1e-10f;
}

/*============================================================================
 * Core Measurement
 *============================================================================*/

static void measure_tone(tone_tracker_t *tt) {
    /* Apply window and load FFT input */
    for (int i = 0; i < TONE_FFT_SIZE; i++) {
        int idx = (tt->buffer_idx + i) % TONE_FFT_SIZE;
        tt->fft_in[i].r = tt->buffer_i[idx] * tt->window[i];
        tt->fft_in[i].i = tt->buffer_q[idx] * tt->window[i];
    }

    /* Run FFT */
    kiss_fft(tt->fft_cfg, tt->fft_in, tt->fft_out);

    /* Calculate magnitudes */
    for (int i = 0; i < TONE_FFT_SIZE; i++) {
        float re = tt->fft_out[i].r;
        float im = tt->fft_out[i].i;
        tt->magnitudes[i] = sqrtf(re * re + im * im);
    }

    /* Special case for DC/carrier (0 Hz) */
    if (tt->nominal_hz < 1.0f) {
        /* Find peak near DC (bin 0) - search both positive and negative freqs */
        int peak_bin = 0;
        float peak_mag = tt->magnitudes[0];

        /* Search positive frequencies (bins 1 to SEARCH_BINS) */
        for (int i = 1; i <= SEARCH_BINS && i < TONE_FFT_SIZE/2; i++) {
            if (tt->magnitudes[i] > peak_mag) {
                peak_mag = tt->magnitudes[i];
                peak_bin = i;
            }
        }

        /* Search negative frequencies (bins FFT_SIZE-1 down to FFT_SIZE-SEARCH_BINS) */
        for (int i = TONE_FFT_SIZE - 1; i >= TONE_FFT_SIZE - SEARCH_BINS; i--) {
            if (tt->magnitudes[i] > peak_mag) {
                peak_mag = tt->magnitudes[i];
                peak_bin = i;
            }
        }

        /* Convert bin to Hz (handle negative frequencies) */
        float peak_frac = parabolic_peak(tt->magnitudes, peak_bin, TONE_FFT_SIZE);
        float measured_hz;
        if (peak_bin < TONE_FFT_SIZE / 2) {
            measured_hz = peak_frac * TONE_HZ_PER_BIN;
        } else {
            measured_hz = (peak_frac - TONE_FFT_SIZE) * TONE_HZ_PER_BIN;
        }

        /* Estimate noise floor (away from carrier) */
        float noise_floor = estimate_noise_floor(tt->magnitudes, TONE_FFT_SIZE, 0, SEARCH_BINS + 5);
        tt->noise_floor_linear = noise_floor;  /* Store for marker detector baseline */
        tt->snr_db = 20.0f * log10f(peak_mag / (noise_floor + 1e-10f));
        tt->valid = (tt->snr_db >= MIN_SNR_DB);

        if (tt->valid) {
            tt->measured_hz = measured_hz;
            tt->offset_hz = measured_hz;  /* Offset from 0 Hz */
            tt->offset_ppm = (tt->offset_hz / 1.0f) * (CARRIER_NOMINAL_HZ / 1e6f);  /* PPM relative to carrier */
        } else {
            tt->measured_hz = 0.0f;
            tt->offset_hz = 0.0f;
            tt->offset_ppm = 0.0f;
        }
        return;
    }

    /* Normal case for 500/600 Hz tones */

    /* Find expected bin locations */
    int nominal_bin = (int)(tt->nominal_hz / TONE_HZ_PER_BIN + 0.5f);
    int lsb_center = TONE_FFT_SIZE - nominal_bin;

    /* Find USB peak (positive frequency) */
    int usb_peak_bin = find_peak_bin(tt->magnitudes,
                                      nominal_bin - SEARCH_BINS,
                                      nominal_bin + SEARCH_BINS,
                                      TONE_FFT_SIZE);
    float usb_peak_frac = parabolic_peak(tt->magnitudes, usb_peak_bin, TONE_FFT_SIZE);
    float usb_peak_mag = tt->magnitudes[usb_peak_bin];

    /* Find LSB peak (negative frequency) */
    int lsb_peak_bin = find_peak_bin(tt->magnitudes,
                                      lsb_center - SEARCH_BINS,
                                      lsb_center + SEARCH_BINS,
                                      TONE_FFT_SIZE);
    float lsb_peak_frac = parabolic_peak(tt->magnitudes, lsb_peak_bin, TONE_FFT_SIZE);
    float lsb_peak_mag = tt->magnitudes[lsb_peak_bin];

    /* Estimate noise floor */
    float noise_floor = estimate_noise_floor(tt->magnitudes, TONE_FFT_SIZE,
                                              nominal_bin, SEARCH_BINS + 5);
    tt->noise_floor_linear = noise_floor;  /* Store for marker detector baseline */

    /* Calculate SNR (use stronger sideband) */
    float peak_mag = (usb_peak_mag > lsb_peak_mag) ? usb_peak_mag : lsb_peak_mag;
    tt->snr_db = 20.0f * log10f(peak_mag / (noise_floor + 1e-10f));

    /* Validity check */
    tt->valid = (tt->snr_db >= MIN_SNR_DB);

    if (tt->valid) {
        /* Sideband spacing method for best accuracy */
        float usb_hz = usb_peak_frac * TONE_HZ_PER_BIN;
        float lsb_hz = (TONE_FFT_SIZE - lsb_peak_frac) * TONE_HZ_PER_BIN;

        /* Average both sidebands */
        tt->measured_hz = (usb_hz + lsb_hz) / 2.0f;
        tt->offset_hz = tt->measured_hz - tt->nominal_hz;

        /* Scale to carrier PPM (offset at tone freq → offset at carrier) */
        tt->offset_ppm = (tt->offset_hz / tt->nominal_hz) * (CARRIER_NOMINAL_HZ / 1e6f);
    } else {
        tt->measured_hz = tt->nominal_hz;
        tt->offset_hz = 0.0f;
        tt->offset_ppm = 0.0f;
    }
}

/*============================================================================
 * Logging
 *============================================================================*/

static void log_measurement(tone_tracker_t *tt) {
    if (!tt->csv_file) return;

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_str[16];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);

    float timestamp_ms = tt->frame_count * TONE_FRAME_MS;

    fprintf(tt->csv_file, "%s,%.1f,%.3f,%.3f,%.2f,%.1f,%s\n",
            time_str,
            timestamp_ms,
            tt->measured_hz,
            tt->offset_hz,
            tt->offset_ppm,
            tt->snr_db,
            tt->valid ? "YES" : "NO");
    fflush(tt->csv_file);
}

/*============================================================================
 * Public API
 *============================================================================*/

tone_tracker_t *tone_tracker_create(float nominal_hz, const char *csv_path) {
    tone_tracker_t *tt = (tone_tracker_t *)calloc(1, sizeof(tone_tracker_t));
    if (!tt) return NULL;

    tt->nominal_hz = nominal_hz;
    tt->start_time = time(NULL);

    /* Allocate buffers */
    tt->buffer_i = (float *)calloc(TONE_FFT_SIZE, sizeof(float));
    tt->buffer_q = (float *)calloc(TONE_FFT_SIZE, sizeof(float));
    tt->fft_in = (kiss_fft_cpx *)malloc(TONE_FFT_SIZE * sizeof(kiss_fft_cpx));
    tt->fft_out = (kiss_fft_cpx *)malloc(TONE_FFT_SIZE * sizeof(kiss_fft_cpx));
    tt->window = (float *)malloc(TONE_FFT_SIZE * sizeof(float));
    tt->magnitudes = (float *)malloc(TONE_FFT_SIZE * sizeof(float));

    if (!tt->buffer_i || !tt->buffer_q || !tt->fft_in ||
        !tt->fft_out || !tt->window || !tt->magnitudes) {
        tone_tracker_destroy(tt);
        return NULL;
    }

    /* Initialize FFT */
    tt->fft_cfg = kiss_fft_alloc(TONE_FFT_SIZE, 0, NULL, NULL);
    if (!tt->fft_cfg) {
        tone_tracker_destroy(tt);
        return NULL;
    }

    /* Generate window */
    generate_blackman_harris(tt->window, TONE_FFT_SIZE);

    /* Open CSV file */
    if (csv_path) {
        tt->csv_file = fopen(csv_path, "w");
        if (tt->csv_file) {
            char time_str[64];
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S",
                     localtime(&tt->start_time));

            fprintf(tt->csv_file, "# Phoenix SDR WWV Tone Tracker (%.0f Hz) v%s\n",
                    nominal_hz, PHOENIX_VERSION_FULL);
            fprintf(tt->csv_file, "# Started: %s\n", time_str);
            fprintf(tt->csv_file, "# FFT: %d-pt, %.2f Hz/bin, %.1f ms frame\n",
                    TONE_FFT_SIZE, TONE_HZ_PER_BIN, TONE_FRAME_MS);
            fprintf(tt->csv_file, "time,timestamp_ms,measured_hz,offset_hz,offset_ppm,snr_db,valid\n");
            fflush(tt->csv_file);
        }
    }

    printf("[TONE] Tracker created for %.0f Hz (%.2f Hz/bin, %.1f ms frame)\n",
           nominal_hz, TONE_HZ_PER_BIN, TONE_FRAME_MS);

    return tt;
}

void tone_tracker_destroy(tone_tracker_t *tt) {
    if (!tt) return;

    if (tt->csv_file) fclose(tt->csv_file);
    if (tt->fft_cfg) kiss_fft_free(tt->fft_cfg);
    free(tt->buffer_i);
    free(tt->buffer_q);
    free(tt->fft_in);
    free(tt->fft_out);
    free(tt->window);
    free(tt->magnitudes);
    free(tt);
}

void tone_tracker_process_sample(tone_tracker_t *tt, float i, float q) {
    if (!tt) return;

    /* Store sample in circular buffer */
    tt->buffer_i[tt->buffer_idx] = i;
    tt->buffer_q[tt->buffer_idx] = q;
    tt->buffer_idx = (tt->buffer_idx + 1) % TONE_FFT_SIZE;
    tt->samples_collected++;

    /* Process when buffer is full */
    if (tt->samples_collected >= TONE_FFT_SIZE) {
        tt->samples_collected = 0;

        measure_tone(tt);
        log_measurement(tt);

        tt->frame_count++;
    }
}

float tone_tracker_get_measured_hz(tone_tracker_t *tt) {
    return tt ? tt->measured_hz : 0.0f;
}

float tone_tracker_get_offset_hz(tone_tracker_t *tt) {
    return tt ? tt->offset_hz : 0.0f;
}

float tone_tracker_get_offset_ppm(tone_tracker_t *tt) {
    return tt ? tt->offset_ppm : 0.0f;
}

float tone_tracker_get_snr_db(tone_tracker_t *tt) {
    return tt ? tt->snr_db : 0.0f;
}

bool tone_tracker_is_valid(tone_tracker_t *tt) {
    return tt ? tt->valid : false;
}

uint64_t tone_tracker_get_frame_count(tone_tracker_t *tt) {
    return tt ? tt->frame_count : 0;
}

float tone_tracker_get_noise_floor(tone_tracker_t *tt) {
    return tt ? tt->noise_floor_linear : 0.0f;
}

/* Global subcarrier noise floor for marker detector */
float g_subcarrier_noise_floor = 0.01f;

void tone_tracker_update_global_noise_floor(tone_tracker_t *tt) {
    if (!tt || !tt->valid) return;

    /* Only update if this tracker has a valid measurement */
    if (tt->noise_floor_linear > 0.0001f) {
        /* Slow adaptation to prevent jumps */
        g_subcarrier_noise_floor += 0.1f * (tt->noise_floor_linear - g_subcarrier_noise_floor);
        if (g_subcarrier_noise_floor < 0.0001f) g_subcarrier_noise_floor = 0.0001f;
    }
}
