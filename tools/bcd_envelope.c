/**
 * @file bcd_envelope.c
 * @brief WWV 100 Hz BCD envelope tracker implementation
 */

#include "bcd_envelope.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*============================================================================
 * Internal State
 *============================================================================*/

struct bcd_envelope {
    bool enabled;

    /* Decimation (12 kHz -> 2.4 kHz) */
    int decim_counter;

    /* Anti-alias lowpass filter (applied at 12 kHz, before decimation)
     * Cutoff 500 Hz rejects 1000 Hz tick energy that would otherwise
     * alias into the 100 Hz detection bin when decimating to 2.4 kHz.
     * Added to fix 100x signal loss - see Option A in signal path analysis. */
    float aa_lpf_i_x1, aa_lpf_i_x2;  /* I channel filter state */
    float aa_lpf_i_y1, aa_lpf_i_y2;
    float aa_lpf_q_x1, aa_lpf_q_x2;  /* Q channel filter state */
    float aa_lpf_q_y1, aa_lpf_q_y2;
    float aa_lpf_b0, aa_lpf_b1, aa_lpf_b2;  /* Filter coefficients (shared) */
    float aa_lpf_a1, aa_lpf_a2;

    /* DC blocker state (y[n] = x[n] - x[n-1] + alpha * y[n-1]) */
    float dc_prev_in_i;
    float dc_prev_in_q;
    float dc_prev_out_i;
    float dc_prev_out_q;

    /* Goertzel state */
    float goertzel_coeff;       /* 2 * cos(2*pi*k/N) */
    float g_s1_i, g_s2_i;       /* I channel state */
    float g_s1_q, g_s2_q;       /* Q channel state */
    int block_index;            /* Sample counter within block */

    /* Envelope tracking */
    float envelope;             /* Smoothed magnitude */
    float envelope_db;

    /* Noise floor estimation (ring buffer of recent magnitudes) */
    float mag_history[256];     /* ~2.5 seconds at 100 Hz update */
    int mag_history_idx;
    int mag_history_count;
    float noise_floor;
    float noise_floor_db;

    /* Current state */
    float snr_db;
    bcd_envelope_status_t status;

    /* Sideband tracking (for symmetry check) */
    float last_pos_mag;
    float last_neg_mag;

    /* Timing */
    uint64_t sample_count;
    uint64_t block_count;

    /* Callback */
    bcd_envelope_callback_fn callback;
    void *user_data;

    /* CSV logging */
    FILE *csv_file;
};

/*============================================================================
 * Anti-Alias Lowpass Filter
 *============================================================================*/

/**
 * Initialize 2nd-order Butterworth lowpass filter coefficients
 * Called once at creation with cutoff=500 Hz, sample_rate=12000 Hz
 */
static void aa_lpf_init(bcd_envelope_t *det, float cutoff_hz, float sample_rate) {
    float w0 = 2.0f * M_PI * cutoff_hz / sample_rate;
    float alpha = sinf(w0) / (2.0f * 0.7071f);  /* Q = 0.7071 for Butterworth */
    float cos_w0 = cosf(w0);

    float a0 = 1.0f + alpha;
    det->aa_lpf_b0 = (1.0f - cos_w0) / 2.0f / a0;
    det->aa_lpf_b1 = (1.0f - cos_w0) / a0;
    det->aa_lpf_b2 = (1.0f - cos_w0) / 2.0f / a0;
    det->aa_lpf_a1 = -2.0f * cos_w0 / a0;
    det->aa_lpf_a2 = (1.0f - alpha) / a0;

    /* Clear filter state */
    det->aa_lpf_i_x1 = det->aa_lpf_i_x2 = 0.0f;
    det->aa_lpf_i_y1 = det->aa_lpf_i_y2 = 0.0f;
    det->aa_lpf_q_x1 = det->aa_lpf_q_x2 = 0.0f;
    det->aa_lpf_q_y1 = det->aa_lpf_q_y2 = 0.0f;
}

/**
 * Process one sample through anti-alias lowpass (I channel)
 */
static inline float aa_lpf_process_i(bcd_envelope_t *det, float x) {
    float y = det->aa_lpf_b0 * x
            + det->aa_lpf_b1 * det->aa_lpf_i_x1
            + det->aa_lpf_b2 * det->aa_lpf_i_x2
            - det->aa_lpf_a1 * det->aa_lpf_i_y1
            - det->aa_lpf_a2 * det->aa_lpf_i_y2;
    det->aa_lpf_i_x2 = det->aa_lpf_i_x1;
    det->aa_lpf_i_x1 = x;
    det->aa_lpf_i_y2 = det->aa_lpf_i_y1;
    det->aa_lpf_i_y1 = y;
    return y;
}

/**
 * Process one sample through anti-alias lowpass (Q channel)
 */
static inline float aa_lpf_process_q(bcd_envelope_t *det, float x) {
    float y = det->aa_lpf_b0 * x
            + det->aa_lpf_b1 * det->aa_lpf_q_x1
            + det->aa_lpf_b2 * det->aa_lpf_q_x2
            - det->aa_lpf_a1 * det->aa_lpf_q_y1
            - det->aa_lpf_a2 * det->aa_lpf_q_y2;
    det->aa_lpf_q_x2 = det->aa_lpf_q_x1;
    det->aa_lpf_q_x1 = x;
    det->aa_lpf_q_y2 = det->aa_lpf_q_y1;
    det->aa_lpf_q_y1 = y;
    return y;
}

/*============================================================================
 * Goertzel Helpers
 *
 * NOTE: Option B correctness optimization - the current implementation runs
 * separate Goertzel filters on I and Q channels, then combines magnitudes.
 * For complex baseband I/Q, the 100 Hz BCD appears as a rotating phasor with
 * energy at both +100 Hz and -100 Hz bins. A proper complex Goertzel would
 * coherently capture this. The current approach works but is suboptimal.
 * See signal path analysis for details.
 *============================================================================*/

/**
 * Initialize Goertzel coefficient for target frequency
 * k = (N * target_freq) / sample_rate
 * coeff = 2 * cos(2 * pi * k / N)
 */
static float goertzel_init_coeff(int block_size, float target_freq, float sample_rate) {
    float k = (block_size * target_freq) / sample_rate;
    float omega = (2.0f * M_PI * k) / block_size;
    return 2.0f * cosf(omega);
}

/**
 * Process one sample through Goertzel
 * s0 = sample + coeff * s1 - s2
 * s2 = s1
 * s1 = s0
 */
static inline void goertzel_process_sample(float sample, float coeff,
                                           float *s1, float *s2) {
    float s0 = sample + coeff * (*s1) - (*s2);
    *s2 = *s1;
    *s1 = s0;
}

/**
 * Compute magnitude at end of block
 * mag = sqrt(s1^2 + s2^2 - coeff*s1*s2)
 */
static float goertzel_magnitude(float s1, float s2, float coeff) {
    float mag_sq = s1*s1 + s2*s2 - coeff*s1*s2;
    return sqrtf(mag_sq > 0 ? mag_sq : 0);
}

/*============================================================================
 * DC Blocker
 *============================================================================*/

/**
 * Single-pole DC blocker: y[n] = x[n] - x[n-1] + alpha * y[n-1]
 * This is a highpass filter with cutoff around (1-alpha)*fs/(2*pi)
 * With alpha=0.995 at 2400 Hz: cutoff ~2 Hz
 */
static inline float dc_block(float input, float *prev_in, float *prev_out, float alpha) {
    float output = input - *prev_in + alpha * (*prev_out);
    *prev_in = input;
    *prev_out = output;
    return output;
}

/*============================================================================
 * Noise Floor Estimation
 *============================================================================*/

/**
 * Estimate noise floor using lower percentile of recent magnitudes
 * This avoids including signal energy in the noise estimate
 */
static float estimate_noise_floor(bcd_envelope_t *det) {
    if (det->mag_history_count < 10) {
        return 1e-6f;  /* Not enough data yet */
    }

    /* Copy and sort (simple insertion sort for small N) */
    float sorted[256];
    int n = det->mag_history_count;
    if (n > 256) n = 256;

    memcpy(sorted, det->mag_history, n * sizeof(float));

    for (int i = 1; i < n; i++) {
        float key = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > key) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = key;
    }

    /* Take lower percentile */
    int idx = (n * BCD_ENV_NOISE_PERCENTILE) / 100;
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;

    return sorted[idx];
}

/*============================================================================
 * Public API Implementation
 *============================================================================*/

bcd_envelope_t *bcd_envelope_create(const char *csv_path) {
    bcd_envelope_t *det = calloc(1, sizeof(bcd_envelope_t));
    if (!det) return NULL;

    det->enabled = true;

    /* Initialize anti-alias lowpass filter (500 Hz cutoff at 12 kHz input rate)
     * This rejects 500/600/1000 Hz tones that would alias into the 100 Hz
     * detection bin when decimating from 12 kHz to 2.4 kHz */
    aa_lpf_init(det, 500.0f, 12000.0f);

    /* Initialize Goertzel coefficient */
    det->goertzel_coeff = goertzel_init_coeff(BCD_ENV_BLOCK_SIZE,
                                               BCD_ENV_TARGET_FREQ_HZ,
                                               BCD_ENV_SAMPLE_RATE);

    /* Initialize state */
    det->envelope = 0.0f;
    det->noise_floor = 1e-6f;
    det->status = BCD_ENV_ABSENT;

    /* Open CSV if requested */
    if (csv_path) {
        det->csv_file = fopen(csv_path, "w");
        if (det->csv_file) {
            fprintf(det->csv_file, "timestamp_ms,envelope,envelope_db,noise_floor_db,"
                                   "snr_db,status,pos_mag,neg_mag\n");
        }
    }

    printf("[bcd_envelope] Created: target=%d Hz, block=%d samples (%.1f ms)\n",
           BCD_ENV_TARGET_FREQ_HZ, BCD_ENV_BLOCK_SIZE,
           1000.0f * BCD_ENV_BLOCK_SIZE / BCD_ENV_SAMPLE_RATE);
    printf("[bcd_envelope] Goertzel coeff=%.6f, DC alpha=%.4f\n",
           det->goertzel_coeff, BCD_ENV_DC_ALPHA);

    return det;
}

void bcd_envelope_destroy(bcd_envelope_t *det) {
    if (!det) return;

    if (det->csv_file) {
        fclose(det->csv_file);
    }

    free(det);
}

void bcd_envelope_set_callback(bcd_envelope_t *det,
                               bcd_envelope_callback_fn callback,
                               void *user_data) {
    if (!det) return;
    det->callback = callback;
    det->user_data = user_data;
}

/**
 * Process one sample at 12 kHz, decimate to 2.4 kHz internally
 */
void bcd_envelope_process_sample(bcd_envelope_t *det,
                                 float i_sample, float q_sample) {
    if (!det || !det->enabled) return;

    /* Anti-alias filter BEFORE decimation (at 12 kHz rate)
     * This is critical - without it, 1000 Hz tick energy aliases into
     * the 100 Hz bin when decimating to 2.4 kHz, causing 100x signal loss */
    float i_filtered = aa_lpf_process_i(det, i_sample);
    float q_filtered = aa_lpf_process_q(det, q_sample);

    /* Decimate: keep every 5th sample (12 kHz -> 2.4 kHz) */
    det->decim_counter++;
    if (det->decim_counter < BCD_ENV_DECIMATION) {
        return;  /* Skip this sample */
    }
    det->decim_counter = 0;

    /* Process at 2.4 kHz */
    bcd_envelope_process_sample_2400(det, i_filtered, q_filtered);
}

/**
 * Process one sample at 2.4 kHz
 */
void bcd_envelope_process_sample_2400(bcd_envelope_t *det,
                                      float i_sample, float q_sample) {
    if (!det || !det->enabled) return;

    det->sample_count++;

    /* DC blocking */
    float i_blocked = dc_block(i_sample, &det->dc_prev_in_i, &det->dc_prev_out_i,
                               BCD_ENV_DC_ALPHA);
    float q_blocked = dc_block(q_sample, &det->dc_prev_in_q, &det->dc_prev_out_q,
                               BCD_ENV_DC_ALPHA);

    /* Feed to Goertzel (I and Q channels separately) */
    goertzel_process_sample(i_blocked, det->goertzel_coeff,
                            &det->g_s1_i, &det->g_s2_i);
    goertzel_process_sample(q_blocked, det->goertzel_coeff,
                            &det->g_s1_q, &det->g_s2_q);

    det->block_index++;

    /* End of block - compute magnitude */
    if (det->block_index >= BCD_ENV_BLOCK_SIZE) {
        det->block_index = 0;
        det->block_count++;

        /* Compute magnitude for I and Q */
        float mag_i = goertzel_magnitude(det->g_s1_i, det->g_s2_i, det->goertzel_coeff);
        float mag_q = goertzel_magnitude(det->g_s1_q, det->g_s2_q, det->goertzel_coeff);

        /* Combined magnitude (both sidebands) */
        /* For real signal, I and Q should have similar 100 Hz content */
        float magnitude = sqrtf(mag_i * mag_i + mag_q * mag_q);

        /* Track sidebands for balance check */
        det->last_pos_mag = mag_i;
        det->last_neg_mag = mag_q;

        /* Reset Goertzel state for next block */
        det->g_s1_i = det->g_s2_i = 0;
        det->g_s1_q = det->g_s2_q = 0;

        /* Update magnitude history for noise estimation */
        det->mag_history[det->mag_history_idx] = magnitude;
        det->mag_history_idx = (det->mag_history_idx + 1) % 256;
        if (det->mag_history_count < 256) det->mag_history_count++;

        /* Update noise floor estimate (every ~10 blocks) */
        if ((det->block_count % 10) == 0) {
            det->noise_floor = estimate_noise_floor(det);
            det->noise_floor_db = 20.0f * log10f(det->noise_floor + 1e-10f);
        }

        /* Smooth envelope */
        det->envelope = BCD_ENV_ALPHA * magnitude +
                        (1.0f - BCD_ENV_ALPHA) * det->envelope;
        det->envelope_db = 20.0f * log10f(det->envelope + 1e-10f);

        /* Calculate SNR */
        det->snr_db = det->envelope_db - det->noise_floor_db;

        /* Update status */
        if (det->snr_db < 0) {
            det->status = BCD_ENV_ABSENT;
        } else if (det->snr_db < BCD_ENV_MIN_SNR_DB) {
            det->status = BCD_ENV_WEAK;
        } else if (det->snr_db < BCD_ENV_GOOD_SNR_DB) {
            det->status = BCD_ENV_PRESENT;
        } else {
            det->status = BCD_ENV_STRONG;
        }

        /* Fire callback */
        if (det->callback) {
            bcd_envelope_frame_t frame = {
                .timestamp_ms = (float)det->block_count * 10.0f,  /* 10 ms per block */
                .envelope = det->envelope,
                .envelope_db = det->envelope_db,
                .noise_floor_db = det->noise_floor_db,
                .snr_db = det->snr_db,
                .status = det->status
            };
            det->callback(&frame, det->user_data);
        }

        /* CSV logging */
        if (det->csv_file) {
            fprintf(det->csv_file, "%.1f,%.6f,%.2f,%.2f,%.2f,%d,%.6f,%.6f\n",
                    (float)det->block_count * 10.0f,
                    det->envelope,
                    det->envelope_db,
                    det->noise_floor_db,
                    det->snr_db,
                    det->status,
                    det->last_pos_mag,
                    det->last_neg_mag);
        }
    }
}

void bcd_envelope_set_enabled(bcd_envelope_t *det, bool enabled) {
    if (det) det->enabled = enabled;
}

bool bcd_envelope_get_enabled(bcd_envelope_t *det) {
    return det ? det->enabled : false;
}

float bcd_envelope_get_snr_db(bcd_envelope_t *det) {
    return det ? det->snr_db : -100.0f;
}

float bcd_envelope_get_envelope(bcd_envelope_t *det) {
    return det ? det->envelope : 0.0f;
}

bcd_envelope_status_t bcd_envelope_get_status(bcd_envelope_t *det) {
    return det ? det->status : BCD_ENV_ABSENT;
}

float bcd_envelope_get_noise_floor_db(bcd_envelope_t *det) {
    return det ? det->noise_floor_db : -100.0f;
}

bool bcd_envelope_get_sideband_balance(bcd_envelope_t *det,
                                       float *pos_mag, float *neg_mag) {
    if (!det) return false;

    if (pos_mag) *pos_mag = det->last_pos_mag;
    if (neg_mag) *neg_mag = det->last_neg_mag;

    /* Check if sidebands are within 3 dB of each other */
    float ratio = det->last_pos_mag / (det->last_neg_mag + 1e-10f);
    return (ratio > 0.7f && ratio < 1.4f);
}

void bcd_envelope_print_stats(bcd_envelope_t *det) {
    if (!det) return;

    const char *status_str;
    switch (det->status) {
        case BCD_ENV_ABSENT:  status_str = "ABSENT";  break;
        case BCD_ENV_WEAK:    status_str = "WEAK";    break;
        case BCD_ENV_PRESENT: status_str = "PRESENT"; break;
        case BCD_ENV_STRONG:  status_str = "STRONG";  break;
        default: status_str = "UNKNOWN";
    }

    printf("[bcd_envelope] Status: %s, SNR: %.1f dB, Envelope: %.2f dB, "
           "Noise: %.2f dB, Blocks: %llu\n",
           status_str, det->snr_db, det->envelope_db, det->noise_floor_db,
           (unsigned long long)det->block_count);
}
