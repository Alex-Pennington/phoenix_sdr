/**
 * @file wwv_sync.c
 * @brief WWV timing synchronization detector
 * 
 * Detects minute markers (800ms) and second ticks (5ms) from WWV recordings.
 * 
 * Two detection modes:
 *   Mode 1 (default): Look for 1000 Hz tone DROPOUT (sustained drop in 1000 Hz energy)
 *                     This works when background has harmonics at 1000 Hz
 *   Mode 2 (-w flag):  Wideband envelope, look for 40ms silence after minute marker
 * 
 * Based on research from:
 *   - NTP driver36 (Dave Mills' WWV/H Audio Demodulator)
 *   - fldigi WWV mode
 *   - NIST Special Publication 432
 * 
 * Usage: wwv_sync <file.iqr> [start_sec] [duration_sec] [-w]
 */

#include "iq_recorder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Detection parameters */
#define BASELINE_WINDOW_MS   2000    /* Running baseline window */
#define SMOOTH_WINDOW_MS     20      /* Envelope smoothing */
#define DROP_THRESHOLD       0.85f   /* Threshold for dropout detection (ratio < this) */
#define RISE_THRESHOLD       1.15f   /* Threshold for rise detection (ratio > this) - lowered for weak signals */
#define MIN_MARKER_MS        400     /* Minimum duration to be a minute marker (was 500) */
#define MAX_MARKER_MS        1100    /* Maximum expected marker duration (was 1000) */
#define SILENCE_THRESHOLD    0.6f    /* Threshold for silence detection in wideband mode */

/* Adaptive detection parameters */
#define LOCAL_WINDOW_MS      500     /* Short window for local baseline */
#define REF_WINDOW_MS        3000    /* Reference window for statistics */
#define REF_GAP_MS           200     /* Gap between current sample and reference window */
#define SIGMA_THRESHOLD      1.5f    /* Standard deviations above local mean */
#define MIN_RISE_RATIO       1.05f   /* Minimum rise ratio to trigger */
#define HYSTERESIS_MS        50      /* Debounce time for state changes */

#define MAX_ENVELOPE_SAMPLES (90 * 60 * 1000)  /* 90 minutes at 1ms resolution */
#define MAX_CANDIDATES       64  /* Increased from 32 for more sensitive detection */

typedef struct {
    float b0, b1, b2;
    float a1, a2;
    float x1, x2;
    float y1, y2;
} biquad_t;

static void biquad_design_bp(biquad_t *bq, float fs, float fc, float Q) {
    float w0 = 2.0f * M_PI * fc / fs;
    float alpha = sinf(w0) / (2.0f * Q);
    
    float b0 = alpha;
    float b1 = 0.0f;
    float b2 = -alpha;
    float a0 = 1.0f + alpha;
    float a1 = -2.0f * cosf(w0);
    float a2 = 1.0f - alpha;
    
    bq->b0 = b0 / a0;
    bq->b1 = b1 / a0;
    bq->b2 = b2 / a0;
    bq->a1 = a1 / a0;
    bq->a2 = a2 / a0;
    
    bq->x1 = bq->x2 = 0.0f;
    bq->y1 = bq->y2 = 0.0f;
}

static float biquad_process(biquad_t *bq, float x) {
    float y = bq->b0 * x + bq->b1 * bq->x1 + bq->b2 * bq->x2
                        - bq->a1 * bq->y1 - bq->a2 * bq->y2;
    bq->x2 = bq->x1;
    bq->x1 = x;
    bq->y2 = bq->y1;
    bq->y1 = y;
    return y;
}

typedef struct {
    float level;
    float attack;
    float decay;
} envelope_t;

static void envelope_init(envelope_t *env, float attack, float decay) {
    env->level = 0.0f;
    env->attack = attack;
    env->decay = decay;
}

static float envelope_process(envelope_t *env, float x) {
    float mag = fabsf(x);
    if (mag > env->level) {
        env->level += env->attack * (mag - env->level);
    } else {
        env->level += env->decay * (mag - env->level);
    }
    return env->level;
}

/*
 * Goertzel Algorithm
 * Single-bin DFT - much more efficient than FFT for detecting one frequency
 * 
 * For block of N samples at sample rate fs, detects frequency f:
 *   k = (f * N) / fs
 *   coeff = 2 * cos(2 * pi * k / N)
 * 
 * Recursion (for each sample):
 *   s[n] = x[n] + coeff * s[n-1] - s[n-2]
 * 
 * After N samples:
 *   magnitude^2 = s1^2 + s2^2 - coeff * s1 * s2
 */
typedef struct {
    float coeff;        /* 2 * cos(2*pi*k/N) */
    float s1, s2;       /* State variables */
    int block_size;     /* N samples per block */
    int sample_count;   /* Samples processed in current block */
} goertzel_t;

static void goertzel_init(goertzel_t *g, float sample_rate, float target_freq, int block_size) {
    float k = (target_freq * block_size) / sample_rate;
    g->coeff = 2.0f * cosf(2.0f * M_PI * k / block_size);
    g->s1 = 0.0f;
    g->s2 = 0.0f;
    g->block_size = block_size;
    g->sample_count = 0;
}

static void goertzel_reset(goertzel_t *g) {
    g->s1 = 0.0f;
    g->s2 = 0.0f;
    g->sample_count = 0;
}

/* Process one sample, returns -1 if block not complete, magnitude if complete */
static float goertzel_process(goertzel_t *g, float sample) {
    /* Goertzel recursion */
    float s0 = sample + g->coeff * g->s1 - g->s2;
    g->s2 = g->s1;
    g->s1 = s0;
    g->sample_count++;
    
    if (g->sample_count >= g->block_size) {
        /* Block complete - compute magnitude */
        float mag_sq = g->s1 * g->s1 + g->s2 * g->s2 - g->coeff * g->s1 * g->s2;
        float mag = sqrtf(fabsf(mag_sq));  /* fabsf handles numerical issues */
        
        /* Reset for next block */
        goertzel_reset(g);
        
        return mag;
    }
    
    return -1.0f;  /* Block not complete */
}

/*
 * Sliding Goertzel with overlapping blocks
 * Processes samples continuously, outputs magnitude every step_size samples
 */
typedef struct {
    float coeff;
    float *buffer;      /* Circular buffer of samples */
    int block_size;     /* N */
    int step_size;      /* Output every step_size samples */
    int buf_pos;        /* Current position in buffer */
    int step_count;     /* Samples since last output */
    int initialized;    /* Have we filled the buffer once? */
} sliding_goertzel_t;

static void sliding_goertzel_init(sliding_goertzel_t *sg, float sample_rate, 
                                   float target_freq, int block_size, int step_size) {
    float k = (target_freq * block_size) / sample_rate;
    sg->coeff = 2.0f * cosf(2.0f * M_PI * k / block_size);
    sg->buffer = calloc(block_size, sizeof(float));
    sg->block_size = block_size;
    sg->step_size = step_size;
    sg->buf_pos = 0;
    sg->step_count = 0;
    sg->initialized = 0;
}

static void sliding_goertzel_free(sliding_goertzel_t *sg) {
    if (sg->buffer) {
        free(sg->buffer);
        sg->buffer = NULL;
    }
}

/* Process one sample. Returns magnitude if output ready, -1 otherwise */
static float sliding_goertzel_process(sliding_goertzel_t *sg, float sample) {
    /* Store sample in circular buffer */
    sg->buffer[sg->buf_pos] = sample;
    sg->buf_pos = (sg->buf_pos + 1) % sg->block_size;
    sg->step_count++;
    
    /* Check if buffer is filled */
    if (!sg->initialized) {
        if (sg->buf_pos == 0) {
            sg->initialized = 1;
        } else {
            return -1.0f;
        }
    }
    
    /* Output every step_size samples */
    if (sg->step_count >= sg->step_size) {
        sg->step_count = 0;
        
        /* Run Goertzel on the full buffer */
        float s1 = 0.0f, s2 = 0.0f;
        int idx = sg->buf_pos;  /* Start from oldest sample */
        
        for (int i = 0; i < sg->block_size; i++) {
            float s0 = sg->buffer[idx] + sg->coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
            idx = (idx + 1) % sg->block_size;
        }
        
        float mag_sq = s1 * s1 + s2 * s2 - sg->coeff * s1 * s2;
        return sqrtf(fabsf(mag_sq));
    }
    
    return -1.0f;
}

/* Minute marker candidate */
typedef struct {
    int start_ms;       /* Start time in ms from file start */
    int end_ms;         /* End time */
    int duration_ms;    /* Duration */
    float peak_ratio;   /* Peak envelope / baseline ratio (or min for dropout) */
    float confidence;   /* Detection confidence 0-1 */
    int tick_count;     /* Number of second ticks found */
    float avg_ratio;    /* Average tick/gap ratio */
    int sec29_silent;   /* Was second 29 silent? */
    int sec59_silent;   /* Was second 59 silent? */
    int mode;           /* 0=dropout, 1=rise, 2=wideband */
} marker_candidate_t;

/* Global envelope buffer (1ms resolution) */
static float *g_envelope = NULL;
static float *g_baseline = NULL;
static float *g_smooth = NULL;
static float *g_local_mean = NULL;
static float *g_local_std = NULL;
static float *g_adaptive_thresh = NULL;
static int g_env_count = 0;
static int g_wideband = 0;   /* 0=1000Hz bandpass, 1=wideband */
static int g_goertzel = 0;   /* 0=biquad, 1=goertzel */
static float g_tone_freq = 1000.0f;  /* Target tone frequency (1000Hz default, 600Hz for 5MHz) */

/* Goertzel parameters */
#define GOERTZEL_BLOCK_SIZE  240   /* 5ms at 48kHz = 200Hz resolution */
#define GOERTZEL_STEP_SIZE   48    /* 1ms step = 1ms output resolution */
#define GOERTZEL_TARGET_FREQ 1000.0f

#define CHUNK_SIZE 4096

/**
 * Extract envelope at 1ms resolution
 * Supports three modes:
 *   - Wideband: raw IQ magnitude envelope
 *   - Biquad: IIR bandpass at 1000Hz + envelope
 *   - Goertzel: single-bin DFT at 1000Hz (most precise)
 */
static int extract_envelope(iqr_reader_t *reader, double start_sec, double duration_sec) {
    const iqr_header_t *hdr = iqr_get_header(reader);
    float fs = (float)hdr->sample_rate_hz;
    
    uint64_t start_sample = (uint64_t)(start_sec * fs);
    uint64_t end_sample = (uint64_t)((start_sec + duration_sec) * fs);
    if (end_sample > hdr->sample_count) end_sample = hdr->sample_count;
    
    int total_ms = (int)((end_sample - start_sample) / (fs / 1000.0));
    if (total_ms > MAX_ENVELOPE_SAMPLES) total_ms = MAX_ENVELOPE_SAMPLES;
    
    g_envelope = calloc(total_ms, sizeof(float));
    g_baseline = calloc(total_ms, sizeof(float));
    g_smooth = calloc(total_ms, sizeof(float));
    g_local_mean = calloc(total_ms, sizeof(float));
    g_local_std = calloc(total_ms, sizeof(float));
    g_adaptive_thresh = calloc(total_ms, sizeof(float));
    if (!g_envelope || !g_baseline || !g_smooth || !g_local_mean || !g_local_std || !g_adaptive_thresh) {
        fprintf(stderr, "Failed to allocate envelope buffers\n");
        return -1;
    }
    
    iqr_seek(reader, start_sample);
    
    /* Setup filters based on mode */
    biquad_t bp_wwv;
    sliding_goertzel_t goertzel;
    envelope_t env;
    
    if (g_wideband) {
        /* Wideband: just envelope follower */
        envelope_init(&env, 0.6f, 0.05f);
    } else if (g_goertzel) {
        /* Goertzel: sliding DFT at target frequency
         * Block size 240 samples at 48kHz = 5ms window = 200Hz resolution
         * Step size 48 samples = 1ms output rate */
        int block_size = (int)(fs * 0.005f);  /* 5ms */
        int step_size = (int)(fs * 0.001f);   /* 1ms */
        sliding_goertzel_init(&goertzel, fs, g_tone_freq, block_size, step_size);
        printf("  Goertzel: block=%d samples (%.1fms), step=%d samples (%.1fms)\n",
               block_size, 1000.0f * block_size / fs,
               step_size, 1000.0f * step_size / fs);
        printf("  Target: %.0f Hz, Resolution: %.1f Hz\n", g_tone_freq, fs / block_size);
    } else {
        /* Biquad bandpass */
        biquad_design_bp(&bp_wwv, fs, g_tone_freq, 2.0f);
        envelope_init(&env, 0.6f, 0.05f);
    }
    
    float dc_prev_in = 0.0f, dc_prev_out = 0.0f;
    
    int16_t xi[CHUNK_SIZE], xq[CHUNK_SIZE];
    uint32_t num_read;
    uint64_t sample_num = start_sample;
    
    int output_interval = (int)(fs * 0.001);  /* 1ms (for non-Goertzel modes) */
    int sample_counter = 0;
    int ms_index = 0;
    float level_accum = 0;  /* For averaging within interval */
    int accum_count = 0;
    
    while (sample_num < end_sample && ms_index < total_ms) {
        iqr_error_t err = iqr_read(reader, xi, xq, CHUNK_SIZE, &num_read);
        if (err != IQR_OK || num_read == 0) break;
        
        for (uint32_t i = 0; i < num_read && sample_num + i < end_sample; i++) {
            float fi = (float)xi[i];
            float fq = (float)xq[i];
            float iq_mag = sqrtf(fi*fi + fq*fq) / 32768.0f;
            
            if (g_wideband) {
                /* Wideband: just use IQ magnitude directly */
                float level = envelope_process(&env, iq_mag);
                sample_counter++;
                if (sample_counter >= output_interval) {
                    if (ms_index < total_ms) {
                        g_envelope[ms_index] = level;
                        ms_index++;
                    }
                    sample_counter = 0;
                }
            } else if (g_goertzel) {
                /* Goertzel mode: DC removal then Goertzel */
                float audio = iq_mag - dc_prev_in + 0.995f * dc_prev_out;
                dc_prev_in = iq_mag;
                dc_prev_out = audio;
                
                /* Sliding Goertzel outputs at 1ms intervals */
                float mag = sliding_goertzel_process(&goertzel, audio);
                if (mag >= 0 && ms_index < total_ms) {
                    g_envelope[ms_index] = mag;
                    ms_index++;
                }
            } else {
                /* Biquad mode: DC removal + bandpass + envelope */
                float audio = iq_mag - dc_prev_in + 0.995f * dc_prev_out;
                dc_prev_in = iq_mag;
                dc_prev_out = audio;
                
                float filt = biquad_process(&bp_wwv, audio);
                float level = envelope_process(&env, filt);
                
                sample_counter++;
                if (sample_counter >= output_interval) {
                    if (ms_index < total_ms) {
                        g_envelope[ms_index] = level;
                        ms_index++;
                    }
                    sample_counter = 0;
                }
            }
        }
        sample_num += num_read;
    }
    
    /* Cleanup Goertzel if used */
    if (g_goertzel) {
        sliding_goertzel_free(&goertzel);
    }
    
    g_env_count = ms_index;
    return 0;
}

/**
 * Compute running baseline using trailing window only
 * (avoids polluting baseline with current marker)
 */
static void compute_baseline(void) {
    for (int i = 0; i < g_env_count; i++) {
        /* Use trailing samples only, with 100ms gap */
        int end = i - 100;
        int start = end - BASELINE_WINDOW_MS;
        if (start < 0) start = 0;
        if (end < 0) end = 0;
        
        if (start >= end) {
            /* Not enough history - use what we have */
            float sum = 0;
            int count = 0;
            for (int j = 0; j <= i; j++) {
                sum += g_envelope[j];
                count++;
            }
            g_baseline[i] = (count > 0) ? sum / count : g_envelope[i];
        } else {
            float sum = 0;
            int count = 0;
            for (int j = start; j <= end; j++) {
                sum += g_envelope[j];
                count++;
            }
            g_baseline[i] = sum / count;
        }
    }
}

/**
 * Smooth the envelope for marker detection
 */
static void smooth_envelope(void) {
    int half_window = SMOOTH_WINDOW_MS / 2;
    
    for (int i = 0; i < g_env_count; i++) {
        int start = i - half_window;
        int end = i + half_window;
        if (start < 0) start = 0;
        if (end >= g_env_count) end = g_env_count - 1;
        
        float sum = 0;
        int count = 0;
        for (int j = start; j <= end; j++) {
            sum += g_envelope[j];
            count++;
        }
        g_smooth[i] = sum / count;
    }
}

/**
 * Compute adaptive threshold using local statistics
 * Uses a reference window (trailing, with gap) to compute mean and stddev
 * Threshold = local_mean + sigma_threshold * local_std
 */
static void compute_adaptive_threshold(void) {
    for (int i = 0; i < g_env_count; i++) {
        /* Reference window: [i - REF_GAP_MS - REF_WINDOW_MS, i - REF_GAP_MS] */
        int ref_end = i - REF_GAP_MS;
        int ref_start = ref_end - REF_WINDOW_MS;
        
        if (ref_start < 0) ref_start = 0;
        if (ref_end < 0) ref_end = 0;
        
        int count = ref_end - ref_start + 1;
        if (count < 100) {
            /* Not enough data yet - use simple defaults */
            g_local_mean[i] = g_smooth[i];
            g_local_std[i] = g_smooth[i] * 0.1f;  /* Assume 10% variation */
            g_adaptive_thresh[i] = g_local_mean[i] * 1.15f;
            continue;
        }
        
        /* Compute mean */
        float sum = 0;
        for (int j = ref_start; j <= ref_end; j++) {
            sum += g_smooth[j];
        }
        float mean = sum / count;
        
        /* Compute standard deviation */
        float var_sum = 0;
        for (int j = ref_start; j <= ref_end; j++) {
            float diff = g_smooth[j] - mean;
            var_sum += diff * diff;
        }
        float std = sqrtf(var_sum / count);
        
        g_local_mean[i] = mean;
        g_local_std[i] = std;
        
        /* Adaptive threshold: mean + k*sigma, but at least MIN_RISE_RATIO * mean */
        float sigma_thresh = mean + SIGMA_THRESHOLD * std;
        float ratio_thresh = mean * MIN_RISE_RATIO;
        g_adaptive_thresh[i] = (sigma_thresh > ratio_thresh) ? sigma_thresh : ratio_thresh;
    }
}

/**
 * Find minute marker candidates - DROPOUT mode
 * Look for sustained drop below baseline (inverted from original)
 */
static int find_markers_dropout(marker_candidate_t *candidates, int max_candidates) {
    int num_candidates = 0;
    int in_marker = 0;
    int marker_start = 0;
    float marker_min = 999.0f;
    
    for (int i = 0; i < g_env_count && num_candidates < max_candidates; i++) {
        float ratio = g_smooth[i] / (g_baseline[i] + 1e-9f);
        
        if (!in_marker && ratio < DROP_THRESHOLD) {
            /* Falling edge - start of potential dropout */
            in_marker = 1;
            marker_start = i;
            marker_min = ratio;
        } else if (in_marker && ratio < marker_min) {
            marker_min = ratio;
        } else if (in_marker && ratio >= DROP_THRESHOLD) {
            /* Rising edge - end of dropout */
            int duration = i - marker_start;
            
            if (duration >= MIN_MARKER_MS && duration <= MAX_MARKER_MS) {
                marker_candidate_t *c = &candidates[num_candidates];
                c->start_ms = marker_start;
                c->end_ms = i;
                c->duration_ms = duration;
                c->peak_ratio = marker_min;  /* Store min ratio for dropout */
                c->confidence = 0.0f;
                c->tick_count = 0;
                c->avg_ratio = 0;
                c->sec29_silent = 0;
                c->sec59_silent = 0;
                c->mode = 0;  /* dropout mode */
                num_candidates++;
            }
            in_marker = 0;
        }
    }
    
    return num_candidates;
}

/**
 * Find minute marker candidates - RISE mode (original fixed threshold)
 * Look for sustained rise above baseline
 */
static int find_markers_rise(marker_candidate_t *candidates, int max_candidates) {
    int num_candidates = 0;
    int in_marker = 0;
    int marker_start = 0;
    float marker_peak = 0;
    
    for (int i = 0; i < g_env_count && num_candidates < max_candidates; i++) {
        float ratio = g_smooth[i] / (g_baseline[i] + 1e-9f);
        
        if (!in_marker && ratio > RISE_THRESHOLD) {
            in_marker = 1;
            marker_start = i;
            marker_peak = ratio;
        } else if (in_marker && ratio > marker_peak) {
            marker_peak = ratio;
        } else if (in_marker && ratio < RISE_THRESHOLD) {
            int duration = i - marker_start;
            
            if (duration >= MIN_MARKER_MS && duration <= MAX_MARKER_MS) {
                marker_candidate_t *c = &candidates[num_candidates];
                c->start_ms = marker_start;
                c->end_ms = i;
                c->duration_ms = duration;
                c->peak_ratio = marker_peak;
                c->confidence = 0.0f;
                c->tick_count = 0;
                c->avg_ratio = 0;
                c->sec29_silent = 0;
                c->sec59_silent = 0;
                c->mode = 1;  /* rise mode */
                num_candidates++;
            }
            in_marker = 0;
        }
    }
    
    return num_candidates;
}

/**
 * Find minute marker candidates - ADAPTIVE mode
 * Uses local statistics to adapt to varying signal levels
 * Detects sustained rise above adaptive threshold (mean + k*sigma)
 */
static int find_markers_adaptive(marker_candidate_t *candidates, int max_candidates) {
    int num_candidates = 0;
    int in_marker = 0;
    int marker_start = 0;
    float marker_peak_sigma = 0;  /* Peak in terms of sigmas above mean */
    int below_count = 0;  /* Hysteresis counter */
    
    for (int i = 0; i < g_env_count && num_candidates < max_candidates; i++) {
        float value = g_smooth[i];
        float thresh = g_adaptive_thresh[i];
        float mean = g_local_mean[i];
        float std = g_local_std[i];
        
        /* Calculate how many sigmas above mean */
        float sigmas = (std > 1e-9f) ? (value - mean) / std : 0;
        
        if (!in_marker && value > thresh) {
            /* Rising edge detected */
            in_marker = 1;
            marker_start = i;
            marker_peak_sigma = sigmas;
            below_count = 0;
        } else if (in_marker) {
            if (sigmas > marker_peak_sigma) {
                marker_peak_sigma = sigmas;
            }
            
            if (value < thresh) {
                below_count++;
            } else {
                below_count = 0;  /* Reset hysteresis */
            }
            
            /* End marker after hysteresis period below threshold */
            if (below_count >= HYSTERESIS_MS) {
                int duration = (i - HYSTERESIS_MS) - marker_start;
                
                if (duration >= MIN_MARKER_MS && duration <= MAX_MARKER_MS) {
                    marker_candidate_t *c = &candidates[num_candidates];
                    c->start_ms = marker_start;
                    c->end_ms = i - HYSTERESIS_MS;
                    c->duration_ms = duration;
                    c->peak_ratio = marker_peak_sigma;  /* Store peak sigmas */
                    c->confidence = 0.0f;
                    c->tick_count = 0;
                    c->avg_ratio = 0;
                    c->sec29_silent = 0;
                    c->sec59_silent = 0;
                    c->mode = 2;  /* adaptive mode */
                    num_candidates++;
                }
                in_marker = 0;
                below_count = 0;
            }
        }
    }
    
    /* Handle marker that extends to end of data */
    if (in_marker && num_candidates < max_candidates) {
        int duration = g_env_count - marker_start;
        if (duration >= MIN_MARKER_MS && duration <= MAX_MARKER_MS) {
            marker_candidate_t *c = &candidates[num_candidates];
            c->start_ms = marker_start;
            c->end_ms = g_env_count;
            c->duration_ms = duration;
            c->peak_ratio = marker_peak_sigma;
            c->confidence = 0.0f;
            c->tick_count = 0;
            c->avg_ratio = 0;
            c->sec29_silent = 0;
            c->sec59_silent = 0;
            c->mode = 2;
            num_candidates++;
        }
    }
    
    return num_candidates;
}

/**
 * Get average envelope level over a window
 */
static float get_level(int center_ms, int window_ms) {
    int half = window_ms / 2;
    int start = center_ms - half;
    int end = center_ms + half;
    
    if (start < 0) start = 0;
    if (end >= g_env_count) end = g_env_count - 1;
    if (start > end) return 0;
    
    float sum = 0;
    int count = 0;
    for (int i = start; i <= end; i++) {
        sum += g_envelope[i];
        count++;
    }
    
    return (count > 0) ? sum / count : 0;
}

/**
 * Verify minute marker using tick detection
 */
static void verify_marker(marker_candidate_t *c, double start_sec) {
    /* For dropout mode, minute boundary is at END of dropout
       For rise mode, minute boundary is at END of rise */
    int minute_ms = c->end_ms;
    
    #define TICK_WINDOW_MS  20   /* Average over 20ms at tick position */
    #define GAP_WINDOW_MS   100  /* Average over 100ms at mid-second */
    #define TICK_RATIO_MIN  1.10f /* Lower threshold for weak signals */
    
    int tick_count = 0;
    int total_checked = 0;
    float ratio_sum = 0;
    
    float sec29_ratio = 0, sec59_ratio = 0;
    
    printf("  Tick verification (comparing level at tick vs mid-second):\n");
    printf("    ");
    
    for (int sec = 1; sec < 60; sec++) {
        int tick_ms = minute_ms + sec * 1000;
        int gap_ms = tick_ms + 500;
        
        if (gap_ms >= g_env_count) break;
        
        float tick_level = get_level(tick_ms, TICK_WINDOW_MS);
        float gap_level = get_level(gap_ms, GAP_WINDOW_MS);
        float ratio = tick_level / (gap_level + 1e-9f);
        
        if (sec == 29) {
            sec29_ratio = ratio;
            printf("[29:%.2f] ", ratio);
        } else if (sec == 59) {
            sec59_ratio = ratio;
            printf("[59:%.2f] ", ratio);
        } else {
            total_checked++;
            if (ratio > TICK_RATIO_MIN) {
                tick_count++;
                ratio_sum += ratio;
            }
        }
        
        if (sec % 15 == 0 && sec < 59) printf("\n    ");
    }
    printf("\n");
    
    c->tick_count = tick_count;
    c->avg_ratio = (tick_count > 0) ? ratio_sum / tick_count : 0;
    
    c->sec29_silent = (sec29_ratio < TICK_RATIO_MIN);
    c->sec59_silent = (sec59_ratio < TICK_RATIO_MIN);
    
    /* Compute confidence score */
    float conf = 0;
    
    /* Duration close to 800ms */
    float dur_err = fabsf(c->duration_ms - 800) / 800.0f;
    conf += 0.25f * (1.0f - dur_err);
    
    /* Good tick count */
    if (total_checked > 0) {
        float tick_ratio_score = (float)tick_count / (float)total_checked;
        conf += 0.35f * tick_ratio_score;
    }
    
    /* Silent seconds 29 and 59 */
    if (c->sec29_silent) conf += 0.2f;
    if (c->sec59_silent) conf += 0.2f;
    
    c->confidence = conf;
}

/**
 * Check 60-second spacing between candidates and boost confidence for matching pairs
 * Also marks candidates with partners for later filtering
 */
static void check_60sec_spacing(marker_candidate_t *candidates, int num_candidates) {
    #define SPACING_TOLERANCE_MS 2000  /* ±2s tolerance */
    
    for (int i = 0; i < num_candidates; i++) {
        int has_60s_neighbor = 0;
        int best_partner = -1;
        int best_diff = 999999;
        
        for (int j = 0; j < num_candidates; j++) {
            if (i == j) continue;
            
            /* Use START times for more accurate spacing */
            int diff_ms = abs(candidates[i].start_ms - candidates[j].start_ms);
            
            /* Check for ~60 second spacing */
            if (abs(diff_ms - 60000) < SPACING_TOLERANCE_MS) {
                has_60s_neighbor = 1;
                if (abs(diff_ms - 60000) < best_diff) {
                    best_diff = abs(diff_ms - 60000);
                    best_partner = j;
                }
            }
            /* Also check for ~120, ~180 second spacing (multiple minutes) */
            if (abs(diff_ms - 120000) < SPACING_TOLERANCE_MS * 2) {  /* Wider for 2 min */
                has_60s_neighbor = 1;
                if (abs(diff_ms - 120000) < best_diff) {
                    best_diff = abs(diff_ms - 120000);
                    best_partner = j;
                }
            }
            if (abs(diff_ms - 180000) < SPACING_TOLERANCE_MS * 3) {  /* Wider for 3 min */
                has_60s_neighbor = 1;
                if (abs(diff_ms - 180000) < best_diff) {
                    best_diff = abs(diff_ms - 180000);
                    best_partner = j;
                }
            }
        }
        
        if (has_60s_neighbor) {
            candidates[i].confidence += 0.25f;  /* Big bonus for 60s spacing */
            if (candidates[i].confidence > 1.0f) candidates[i].confidence = 1.0f;
            
            /* Store partner info in unused fields */
            /* (We're repurposing avg_ratio as partner index if > 100) */
            if (best_partner >= 0) {
                candidates[i].avg_ratio = 100.0f + best_partner;  /* Encode partner */
            }
        }
    }
}

/**
 * Filter candidates to only keep those with 60-second partners
 * Returns new count of valid candidates
 */
static int filter_by_spacing(marker_candidate_t *candidates, int num_candidates) {
    /* First pass: mark candidates with 60s partners */
    int valid[MAX_CANDIDATES] = {0};
    
    for (int i = 0; i < num_candidates; i++) {
        /* Check if this candidate has a partner (encoded in avg_ratio) */
        if (candidates[i].avg_ratio >= 100.0f) {
            valid[i] = 1;
            int partner = (int)(candidates[i].avg_ratio - 100.0f);
            if (partner >= 0 && partner < num_candidates) {
                valid[partner] = 1;
            }
        }
    }
    
    /* Second pass: compact the array */
    int write_idx = 0;
    for (int i = 0; i < num_candidates; i++) {
        if (valid[i]) {
            /* Reset avg_ratio to real value before copying */
            if (candidates[i].avg_ratio >= 100.0f) {
                candidates[i].avg_ratio = 0;  /* Reset since we used it as temp storage */
            }
            if (write_idx != i) {
                candidates[write_idx] = candidates[i];
            }
            write_idx++;
        }
    }
    
    return write_idx;
}

/**
 * Refine marker edge detection using derivative/gradient analysis
 * Returns refined start time in ms with sub-ms precision as float
 */
static float refine_marker_start(marker_candidate_t *c) {
    /* Search window around the detected start */
    int search_start = c->start_ms - 100;
    int search_end = c->start_ms + 100;
    if (search_start < 0) search_start = 0;
    if (search_end >= g_env_count) search_end = g_env_count - 1;
    
    /* Find the steepest rising edge (maximum positive derivative) */
    float max_derivative = 0;
    int max_derivative_idx = c->start_ms;
    
    for (int i = search_start + 5; i < search_end - 5; i++) {
        /* Compute derivative using 5-point stencil for smoothing */
        float derivative = (g_smooth[i+5] - g_smooth[i-5]) / 10.0f;
        if (derivative > max_derivative) {
            max_derivative = derivative;
            max_derivative_idx = i;
        }
    }
    
    /* Now find the threshold crossing point near the max derivative */
    /* Use the midpoint between pre-marker baseline and marker level */
    float pre_level = get_level(max_derivative_idx - 50, 30);  /* 30ms average before */
    float marker_level = get_level(max_derivative_idx + 50, 30);  /* 30ms average after */
    float threshold = (pre_level + marker_level) / 2.0f;
    
    /* Search for threshold crossing near max derivative */
    int crossing_idx = max_derivative_idx;
    for (int i = max_derivative_idx - 20; i < max_derivative_idx + 20 && i < g_env_count - 1; i++) {
        if (i < 0) continue;
        if (g_smooth[i] < threshold && g_smooth[i+1] >= threshold) {
            /* Linear interpolation for sub-ms precision */
            float frac = (threshold - g_smooth[i]) / (g_smooth[i+1] - g_smooth[i] + 1e-9f);
            return (float)i + frac;
        }
    }
    
    return (float)max_derivative_idx;
}

/**
 * Refine marker edge detection for END of marker
 * Returns refined end time in ms with sub-ms precision as float
 */
static float refine_marker_end(marker_candidate_t *c) {
    /* Search window around the detected end */
    int search_start = c->end_ms - 100;
    int search_end = c->end_ms + 100;
    if (search_start < 0) search_start = 0;
    if (search_end >= g_env_count) search_end = g_env_count - 1;
    
    /* Find the steepest falling edge (maximum negative derivative) */
    float min_derivative = 0;
    int min_derivative_idx = c->end_ms;
    
    for (int i = search_start + 5; i < search_end - 5; i++) {
        float derivative = (g_smooth[i+5] - g_smooth[i-5]) / 10.0f;
        if (derivative < min_derivative) {
            min_derivative = derivative;
            min_derivative_idx = i;
        }
    }
    
    /* Find threshold crossing near falling edge */
    float marker_level = get_level(min_derivative_idx - 50, 30);
    float post_level = get_level(min_derivative_idx + 50, 30);
    float threshold = (marker_level + post_level) / 2.0f;
    
    for (int i = min_derivative_idx - 20; i < min_derivative_idx + 20 && i < g_env_count - 1; i++) {
        if (i < 0) continue;
        if (g_smooth[i] >= threshold && g_smooth[i+1] < threshold) {
            float frac = (threshold - g_smooth[i]) / (g_smooth[i+1] - g_smooth[i] + 1e-9f);
            return (float)i + frac;
        }
    }
    
    return (float)min_derivative_idx;
}

/* Chain of minute markers with consistent 60s spacing */
#define MAX_CHAIN_LENGTH 10
typedef struct {
    int indices[MAX_CHAIN_LENGTH];  /* Indices into candidates array */
    int length;                      /* Number of markers in chain */
    float score;                     /* Overall chain quality score */
    int sec59_silent_count;          /* How many have sec59 silent */
    int sec29_silent_count;          /* How many have sec29 silent */
    float avg_duration_error;        /* Average |duration - 800ms| */
    float avg_confidence;            /* Average confidence */
    int offset_ms;                   /* Estimated offset from file start to minute boundary */
} marker_chain_t;

/**
 * Build chains of candidates with consistent 60-second spacing
 */
static int build_chains(marker_candidate_t *candidates, int num_candidates,
                        marker_chain_t *chains, int max_chains) {
    int num_chains = 0;
    int used[MAX_CANDIDATES] = {0};
    
    /* For each candidate, try to build a chain starting from it */
    for (int start = 0; start < num_candidates && num_chains < max_chains; start++) {
        if (used[start]) continue;
        
        marker_chain_t *chain = &chains[num_chains];
        chain->indices[0] = start;
        chain->length = 1;
        used[start] = 1;
        
        /* Try to extend the chain forward */
        int current = start;
        while (chain->length < MAX_CHAIN_LENGTH) {
            int best_next = -1;
            int best_diff = 999999;
            
            for (int j = 0; j < num_candidates; j++) {
                if (used[j]) continue;
                
                /* Use START times for chain building */
                int diff_ms = candidates[j].start_ms - candidates[current].start_ms;
                
                /* TIGHT tolerance: ~60s forward within 2s (allows for some drift) */
                if (diff_ms > 58000 && diff_ms < 62000) {
                    int err = abs(diff_ms - 60000);
                    if (err < best_diff) {
                        best_diff = err;
                        best_next = j;
                    }
                }
            }
            
            if (best_next < 0) break;
            
            chain->indices[chain->length] = best_next;
            chain->length++;
            used[best_next] = 1;
            current = best_next;
        }
        
        /* Only keep chains with 2+ members */
        if (chain->length >= 2) {
            num_chains++;
        } else {
            used[start] = 0;  /* Release for potential use in another chain */
        }
    }
    
    return num_chains;
}

/**
 * Score a chain based on sec59 silence, duration accuracy, and confidence
 */
static void score_chain(marker_chain_t *chain, marker_candidate_t *candidates) {
    float dur_error_sum = 0;
    float conf_sum = 0;
    int sec59_count = 0;
    int sec29_count = 0;
    
    /* Also compute spacing consistency */
    float spacing_error_sum = 0;
    int spacing_count = 0;
    
    for (int i = 0; i < chain->length; i++) {
        marker_candidate_t *c = &candidates[chain->indices[i]];
        
        dur_error_sum += fabsf(c->duration_ms - 800);
        conf_sum += c->confidence;
        
        if (c->sec59_silent) sec59_count++;
        if (c->sec29_silent) sec29_count++;
        
        /* Check spacing to next marker */
        if (i < chain->length - 1) {
            marker_candidate_t *next = &candidates[chain->indices[i+1]];
            int spacing = next->start_ms - c->start_ms;
            float spacing_err = fabsf(spacing - 60000);
            spacing_error_sum += spacing_err;
            spacing_count++;
        }
    }
    
    chain->sec59_silent_count = sec59_count;
    chain->sec29_silent_count = sec29_count;
    chain->avg_duration_error = dur_error_sum / chain->length;
    chain->avg_confidence = conf_sum / chain->length;
    
    float avg_spacing_error = (spacing_count > 0) ? spacing_error_sum / spacing_count : 0;
    
    /* Compute offset: take the first marker's start_ms modulo 60000 */
    int first_start_ms = candidates[chain->indices[0]].start_ms;
    chain->offset_ms = first_start_ms % 60000;
    
    /* Score formula:
     * - 25 points: sec59 silent ratio (most important - WWV signature)
     * - 10 points: sec29 silent ratio
     * - 15 points: duration accuracy (closer to 800ms is better)
     * - 25 points: chain length bonus (bigger chains = much more confidence)
     * - 15 points: spacing consistency (closer to exactly 60s = better)
     * - 10 points: average confidence
     */
    float score = 0;
    
    /* sec59 silent ratio (25 points max) */
    score += 25.0f * (float)sec59_count / chain->length;
    
    /* sec29 silent ratio (10 points max) */
    score += 10.0f * (float)sec29_count / chain->length;
    
    /* Duration accuracy: 15 points if avg error is 0, decreasing */
    float dur_score = 15.0f * (1.0f - chain->avg_duration_error / 300.0f);
    if (dur_score < 0) dur_score = 0;
    score += dur_score;
    
    /* Chain length bonus (25 points max for 6+ markers, scaled more aggressively) */
    /* 2 markers = 5pts, 3 = 10pts, 4 = 15pts, 5 = 20pts, 6+ = 25pts */
    float len_score = 25.0f * (float)(chain->length - 1) / 5.0f;
    if (len_score > 25.0f) len_score = 25.0f;
    score += len_score;
    
    /* Spacing consistency: 15 points if avg spacing error < 100ms */
    float spacing_score = 15.0f * (1.0f - avg_spacing_error / 2000.0f);
    if (spacing_score < 0) spacing_score = 0;
    score += spacing_score;
    
    /* Average confidence (10 points max) */
    score += 10.0f * chain->avg_confidence;
    
    chain->score = score;
}

/* Global debug flag for verbose pulse measurement */
static int g_pulse_debug = 0;

/**
 * Measure pulse duration at a given second position
 * Returns duration in ms, or 0 if no pulse detected
 * 
 * WWV BCD encoding: The tone is ON for 200ms (binary 0), 500ms (binary 1), 
 * or 800ms (position marker). Silent seconds (29, 59) have no tone.
 * 
 * Energy ratio method:
 * Compare energy in early part of second vs late part.
 * - 200ms pulse: most energy in 0-300ms
 * - 500ms pulse: energy spread 0-600ms  
 * - 800ms pulse: energy through 0-850ms
 * - Silent: low energy throughout
 */
static int measure_pulse_duration(int minute_ms, int second) {
    int tick_ms = minute_ms + second * 1000;
    
    /* Bounds check - need full second of data */
    if (tick_ms < 0 || tick_ms + 1000 >= g_env_count) return 0;
    
    /* Compute energy in different time windows */
    float e_0_200 = 0, e_200_400 = 0, e_400_600 = 0, e_600_800 = 0, e_800_1000 = 0;
    
    for (int i = tick_ms; i < tick_ms + 200 && i < g_env_count; i++)
        e_0_200 += g_envelope[i];
    for (int i = tick_ms + 200; i < tick_ms + 400 && i < g_env_count; i++)
        e_200_400 += g_envelope[i];
    for (int i = tick_ms + 400; i < tick_ms + 600 && i < g_env_count; i++)
        e_400_600 += g_envelope[i];
    for (int i = tick_ms + 600; i < tick_ms + 800 && i < g_env_count; i++)
        e_600_800 += g_envelope[i];
    for (int i = tick_ms + 800; i < tick_ms + 1000 && i < g_env_count; i++)
        e_800_1000 += g_envelope[i];
    
    e_0_200 /= 200;
    e_200_400 /= 200;
    e_400_600 /= 200;
    e_600_800 /= 200;
    e_800_1000 /= 200;
    
    /* Total early energy vs late baseline */
    float total = e_0_200 + e_200_400 + e_400_600 + e_600_800;
    float baseline = e_800_1000;
    
    /* Check for silent second */
    float contrast = (total / 4.0f) / (baseline + 1e-9f);
    if (contrast < 1.2f) {
        if (g_pulse_debug) printf("      Sec %d: SILENT (contrast=%.2f)\n", second, contrast);
        return 0;
    }
    
    /* Determine duration based on energy ratios */
    /* For 200ms pulse: e_0_200 >> e_200_400 */
    /* For 500ms pulse: e_0_200+e_200_400 >> e_400_600+e_600_800 */
    /* For 800ms pulse: all windows have similar energy */
    
    float early = e_0_200 + e_200_400;  /* 0-400ms */
    float mid = e_400_600;               /* 400-600ms */
    float late = e_600_800;              /* 600-800ms */
    
    /* Ratio of early to late parts */
    float early_mid_ratio = early / (mid + baseline + 1e-9f);
    float early_late_ratio = early / (late + baseline + 1e-9f);
    float mid_late_ratio = mid / (late + baseline + 1e-9f);
    
    int duration;
    
    /* Decision tree based on energy distribution */
    if (early_late_ratio > 3.0f && early_mid_ratio > 2.0f) {
        /* Very front-loaded - 200ms pulse */
        duration = 200;
    } else if (early_late_ratio > 1.8f && mid_late_ratio < 1.5f) {
        /* Energy in first half, not much in middle - 200-300ms */
        duration = 250;
    } else if (mid_late_ratio > 1.5f && early_late_ratio < 2.0f) {
        /* Energy extends through middle but drops at end - 500ms pulse */
        duration = 500;
    } else if (late / (baseline + 1e-9f) > 1.5f) {
        /* Energy extends through late period - 800ms marker */
        duration = 800;
    } else {
        /* Unclear - estimate based on weighted sum */
        float weighted = e_0_200 * 100 + e_200_400 * 300 + e_400_600 * 500 + e_600_800 * 700;
        float total_e = e_0_200 + e_200_400 + e_400_600 + e_600_800;
        duration = (int)(weighted / (total_e + 1e-9f));
    }
    
    /* Clamp and validate */
    if (duration < 150) duration = 200;
    if (duration > 850) duration = 800;
    
    if (g_pulse_debug) {
        printf("      Sec %d: dur=%d (e_early=%.4f, e_mid=%.4f, e_late=%.4f, e_base=%.4f)\n",
               second, duration, early, mid, late, baseline);
    }
    
    return duration;
}

/**
 * Classify pulse duration into BCD symbol
 * Returns: 0 = binary 0 (150-350ms), 1 = binary 1 (380-620ms), 
 *          2 = marker (650-950ms), -1 = invalid/silent
 */
static int classify_pulse(int duration_ms) {
    if (duration_ms >= 150 && duration_ms <= 350) return 0;  /* Binary 0 (~200ms) */
    if (duration_ms >= 380 && duration_ms <= 620) return 1;  /* Binary 1 (~500ms) */
    if (duration_ms >= 650 && duration_ms <= 950) return 2;  /* Marker (~800ms) */
    return -1;  /* Invalid/unknown/silent */
}

/**
 * Decode BCD time code from WWV signal
 * 
 * WWV Time Code Format (seconds within minute):
 * Sec 0:     800ms marker (minute reference) - marks the START of minute
 * Sec 1-4:   Minutes units (BCD: 1,2,4,8)
 * Sec 5-8:   Minutes tens (BCD: 10,20,40, unused)
 * Sec 9:     800ms position marker
 * Sec 10-14: Hours units (unused, 1,2,4,8)
 * Sec 15-18: Hours tens (10,20, unused, unused)
 * Sec 19:    800ms position marker
 * Sec 20-24: Day of year units (unused, 1,2,4,8)
 * Sec 25-28: Day of year tens (10,20,40,80)
 * Sec 29:    NO PULSE (silent second)
 * Sec 30-33: Day of year hundreds (100,200, unused, unused)
 * Sec 34-38: DUT1 correction sign and magnitude
 * Sec 39:    800ms position marker
 * Sec 40-44: Year units (1,2,4,8, unused)
 * Sec 45-48: Year tens (10,20,40,80)
 * Sec 49:    800ms position marker
 * Sec 50:    Unused
 * Sec 51:    Leap year indicator
 * Sec 52:    Leap second warning
 * Sec 53-58: DST indicators and unused
 * Sec 59:    NO PULSE (silent second)
 * 
 * Note: Pulses START at the second boundary. The minute marker (sec 0)
 * is an 800ms pulse that starts at the minute boundary.
 */
/* Debug: dump envelope values around a second tick */
static void debug_dump_second(int minute_ms, int second) {
    int tick_ms = minute_ms + second * 1000;
    if (tick_ms < 0 || tick_ms + 1000 >= g_env_count) return;
    
    /* Compute baseline (min) and peak as measure_pulse_duration does */
    float baseline = 999.0f;
    for (int i = tick_ms + 850; i < tick_ms + 950; i++) {
        if (g_envelope[i] < baseline) baseline = g_envelope[i];
    }
    if (baseline > 900.0f) baseline = 0.001f;
    
    float peak = 0;
    for (int i = tick_ms; i < tick_ms + 850 && i < g_env_count - 5; i++) {
        float avg = 0;
        for (int j = 0; j < 5; j++) avg += g_envelope[i + j];
        avg /= 5.0f;
        if (avg > peak) peak = avg;
    }
    
    float contrast = peak / (baseline + 1e-9f);
    printf("    Sec %2d: baseline=%.4f, peak=%.4f, contrast=%.2f\n", 
           second, baseline, peak, contrast);
}

static void decode_bcd_time(int marker_start_ms, int marker_end_ms, double start_sec) {
    printf("\n  ============================================\n");
    printf("  BCD TIME CODE DECODE\n");
    printf("  ============================================\n");
    
    /* Debug: dump envelope stats for first few seconds */
    printf("  Debug: Envelope analysis at marker_start_ms=%d\n", marker_start_ms);
    printf("    Sec  0-19: ");
    for (int s = 0; s < 20; s++) {
        int tick_ms = marker_start_ms + s * 1000;
        if (tick_ms < 0 || tick_ms + 1000 >= g_env_count) { printf("--- "); continue; }
        float baseline = 999.0f;
        for (int i = tick_ms + 850; i < tick_ms + 950; i++)
            if (g_envelope[i] < baseline) baseline = g_envelope[i];
        if (baseline > 900.0f) baseline = 0.001f;
        float peak = 0;
        for (int i = tick_ms; i < tick_ms + 850 && i < g_env_count - 5; i++) {
            float avg = 0;
            for (int j = 0; j < 5; j++) avg += g_envelope[i + j];
            avg /= 5.0f;
            if (avg > peak) peak = avg;
        }
        float contrast = peak / (baseline + 1e-9f);
        printf("%.1f ", contrast);
    }
    printf("\n    Sec 20-39: ");
    for (int s = 20; s < 40; s++) {
        int tick_ms = marker_start_ms + s * 1000;
        if (tick_ms < 0 || tick_ms + 1000 >= g_env_count) { printf("--- "); continue; }
        float baseline = 999.0f;
        for (int i = tick_ms + 850; i < tick_ms + 950; i++)
            if (g_envelope[i] < baseline) baseline = g_envelope[i];
        if (baseline > 900.0f) baseline = 0.001f;
        float peak = 0;
        for (int i = tick_ms; i < tick_ms + 850 && i < g_env_count - 5; i++) {
            float avg = 0;
            for (int j = 0; j < 5; j++) avg += g_envelope[i + j];
            avg /= 5.0f;
            if (avg > peak) peak = avg;
        }
        float contrast = peak / (baseline + 1e-9f);
        printf("%.1f ", contrast);
    }
    printf("\n    Sec 40-59: ");
    for (int s = 40; s < 60; s++) {
        int tick_ms = marker_start_ms + s * 1000;
        if (tick_ms < 0 || tick_ms + 1000 >= g_env_count) { printf("--- "); continue; }
        float baseline = 999.0f;
        for (int i = tick_ms + 850; i < tick_ms + 950; i++)
            if (g_envelope[i] < baseline) baseline = g_envelope[i];
        if (baseline > 900.0f) baseline = 0.001f;
        float peak = 0;
        for (int i = tick_ms; i < tick_ms + 850 && i < g_env_count - 5; i++) {
            float avg = 0;
            for (int j = 0; j < 5; j++) avg += g_envelope[i + j];
            avg /= 5.0f;
            if (avg > peak) peak = avg;
        }
        float contrast = peak / (baseline + 1e-9f);
        printf("%.1f ", contrast);
    }
    printf("\n\n");
    
    /* Search for best alignment by looking for position markers at expected locations */
    /* Try offsets from -500ms to +500ms to find best alignment */
    int best_offset = 0;
    int best_marker_count = 0;
    
    for (int offset = -500; offset <= 500; offset += 50) {
        int test_start = marker_start_ms + offset;
        if (test_start < 0) continue;
        
        int marker_count = 0;
        int position_markers[] = {0, 9, 19, 39, 49};  /* Expected 800ms markers (not 29, 59 which are silent) */
        
        for (int m = 0; m < 5; m++) {
            int dur = measure_pulse_duration(test_start, position_markers[m]);
            if (dur >= 600 && dur <= 950) marker_count++;  /* Looks like 800ms marker */
        }
        
        /* Also check that 29 and 59 are silent */
        int dur29 = measure_pulse_duration(test_start, 29);
        int dur59 = measure_pulse_duration(test_start, 59);
        if (dur29 < 150) marker_count++;
        if (dur59 < 150) marker_count++;
        
        if (marker_count > best_marker_count) {
            best_marker_count = marker_count;
            best_offset = offset;
        }
    }
    
    int minute_start_ms = marker_start_ms + best_offset;
    printf("  Alignment offset: %d ms (found %d/7 markers)\n", best_offset, best_marker_count);
    printf("  Minute starts at: %.3f sec into file\n", start_sec + minute_start_ms / 1000.0);
    
    int pulses[60];
    int durations[60];
    int valid_count = 0;
    
    /* Measure all 60 pulse durations */
    printf("  Pulse durations (ms):\n");
    g_pulse_debug = 1;  /* Enable verbose pulse debug */
    for (int sec = 0; sec < 60; sec++) {
        durations[sec] = measure_pulse_duration(minute_start_ms, sec);
        pulses[sec] = classify_pulse(durations[sec]);
        
        if (pulses[sec] >= 0) valid_count++;
    }
    
    /* Print pulse map */
    printf("    ");
    for (int sec = 0; sec < 60; sec++) {
        char sym = '?';
        if (pulses[sec] == 0) sym = '0';
        else if (pulses[sec] == 1) sym = '1';
        else if (pulses[sec] == 2) sym = 'M';
        else if (sec == 29 || sec == 59) sym = '-';  /* Expected silent */
        
        printf("%c", sym);
        if ((sec + 1) % 10 == 0) printf(" ");
        if ((sec + 1) % 30 == 0 && sec < 59) printf("\n    ");
    }
    printf("\n");
    
    /* Print numeric durations for debugging */
    printf("  Numeric durations:\n    ");
    for (int sec = 0; sec < 60; sec++) {
        printf("%3d ", durations[sec]);
        if ((sec + 1) % 10 == 0) printf("\n    ");
    }
    printf("\n");
    printf("  Legend: 0=200ms, 1=500ms, M=800ms marker, -=silent, ?=invalid\n\n");
    
    /* Verify position markers at expected positions */
    int markers_ok = 0;
    int expected_markers[] = {0, 9, 19, 29, 39, 49, 59};
    printf("  Position markers: ");
    for (int i = 0; i < 7; i++) {
        int sec = expected_markers[i];
        if (sec == 29 || sec == 59) {
            /* These should be silent */
            if (durations[sec] < 100) {
                printf("[%d:silent] ", sec);
                markers_ok++;
            } else {
                printf("[%d:FAIL] ", sec);
            }
        } else {
            /* These should be 800ms markers */
            if (pulses[sec] == 2) {
                printf("[%d:OK] ", sec);
                markers_ok++;
            } else {
                printf("[%d:FAIL] ", sec);
            }
        }
    }
    printf("\n  Markers valid: %d/7\n\n", markers_ok);
    
    if (markers_ok < 4) {
        printf("  WARNING: Insufficient valid markers for reliable decode\n\n");
    }
    
    /* Decode minutes (seconds 1-8) */
    int minutes = 0;
    if (pulses[1] == 1) minutes += 1;
    if (pulses[2] == 1) minutes += 2;
    if (pulses[3] == 1) minutes += 4;
    if (pulses[4] == 1) minutes += 8;
    if (pulses[5] == 1) minutes += 10;
    if (pulses[6] == 1) minutes += 20;
    if (pulses[7] == 1) minutes += 40;
    /* sec 8 is unused for minutes */
    
    /* Decode hours (seconds 10-18) */
    int hours = 0;
    /* sec 10 unused */
    if (pulses[11] == 1) hours += 1;
    if (pulses[12] == 1) hours += 2;
    if (pulses[13] == 1) hours += 4;
    if (pulses[14] == 1) hours += 8;
    if (pulses[15] == 1) hours += 10;
    if (pulses[16] == 1) hours += 20;
    /* sec 17-18 unused for hours */
    
    /* Decode day of year (seconds 20-33) */
    int day = 0;
    /* sec 20 unused */
    if (pulses[21] == 1) day += 1;
    if (pulses[22] == 1) day += 2;
    if (pulses[23] == 1) day += 4;
    if (pulses[24] == 1) day += 8;
    if (pulses[25] == 1) day += 10;
    if (pulses[26] == 1) day += 20;
    if (pulses[27] == 1) day += 40;
    if (pulses[28] == 1) day += 80;
    /* sec 29 is silent */
    if (pulses[30] == 1) day += 100;
    if (pulses[31] == 1) day += 200;
    /* sec 32-33 unused for day */
    
    /* Decode year (seconds 40-48) - last 2 digits */
    int year = 0;
    if (pulses[40] == 1) year += 1;
    if (pulses[41] == 1) year += 2;
    if (pulses[42] == 1) year += 4;
    if (pulses[43] == 1) year += 8;
    /* sec 44 unused */
    if (pulses[45] == 1) year += 10;
    if (pulses[46] == 1) year += 20;
    if (pulses[47] == 1) year += 40;
    if (pulses[48] == 1) year += 80;
    
    /* Decode DUT1 (seconds 34-38) */
    int dut1_sign = (pulses[34] == 1) ? -1 : 1;
    int dut1_value = 0;
    if (pulses[35] == 1) dut1_value += 1;
    if (pulses[36] == 1) dut1_value += 2;
    if (pulses[37] == 1) dut1_value += 4;
    if (pulses[38] == 1) dut1_value += 8;  /* 0.8 correction */
    float dut1 = dut1_sign * dut1_value * 0.1f;
    
    /* Decode flags */
    int leap_year = (pulses[51] == 1);
    int leap_second_warning = (pulses[52] == 1);
    int dst_status = 0;
    if (pulses[53] == 1) dst_status |= 1;
    if (pulses[54] == 1) dst_status |= 2;
    
    /* Sanity checks */
    int time_valid = 1;
    if (minutes > 59) { minutes = -1; time_valid = 0; }
    if (hours > 23) { hours = -1; time_valid = 0; }
    if (day < 1 || day > 366) { day = -1; time_valid = 0; }
    if (year > 99) { year = -1; time_valid = 0; }
    
    /* Print decoded time */
    printf("  DECODED TIME (UTC):\n");
    if (time_valid) {
        printf("    Time: %02d:%02d:00 UTC\n", hours, minutes);
        printf("    Date: Day %03d of 20%02d\n", day, year);
        printf("    DUT1: %+.1f seconds\n", dut1);
        printf("    Leap year: %s\n", leap_year ? "Yes" : "No");
        printf("    Leap second warning: %s\n", leap_second_warning ? "YES" : "No");
        printf("    DST status: %d\n", dst_status);
    } else {
        printf("    Time: %02d:%02d:00 UTC (some values invalid)\n", 
               hours >= 0 ? hours : 0, minutes >= 0 ? minutes : 0);
        printf("    Date: Day %d of 20%02d\n", day, year >= 0 ? year : 0);
        printf("    WARNING: Time decode may be unreliable\n");
    }
    printf("\n");
    
    /* Show raw bit pattern for debugging */
    printf("  Raw BCD bits:\n");
    printf("    Minutes: ");
    for (int i = 1; i <= 8; i++) printf("%d", pulses[i] == 1 ? 1 : 0);
    printf(" -> %d\n", minutes);
    printf("    Hours:   ");
    for (int i = 10; i <= 18; i++) printf("%d", pulses[i] == 1 ? 1 : 0);
    printf(" -> %d\n", hours);
    printf("    Day:     ");
    for (int i = 20; i <= 28; i++) printf("%d", pulses[i] == 1 ? 1 : 0);
    printf("-");
    for (int i = 30; i <= 33; i++) printf("%d", pulses[i] == 1 ? 1 : 0);
    printf(" -> %d\n", day);
    printf("    Year:    ");
    for (int i = 40; i <= 48; i++) printf("%d", pulses[i] == 1 ? 1 : 0);
    printf(" -> %d\n", year);
}

/**
 * Find the best chain and report final timing result
 */
static int select_best_chain(marker_chain_t *chains, int num_chains,
                             marker_candidate_t *candidates, double start_sec) {
    if (num_chains == 0) return -1;
    
    /* Score all chains */
    for (int i = 0; i < num_chains; i++) {
        score_chain(&chains[i], candidates);
    }
    
    /* Find best chain - prefer longer chains when scores are close */
    int best = 0;
    for (int i = 1; i < num_chains; i++) {
        if (chains[i].score > chains[best].score + 5.0f) {
            /* Clear winner by more than 5 points */
            best = i;
        } else if (chains[i].score > chains[best].score - 5.0f) {
            /* Within 5 points - prefer longer chain */
            if (chains[i].length > chains[best].length) {
                best = i;
            }
        }
    }
    
    marker_chain_t *winner = &chains[best];
    
    printf("\n");
    printf("===========================================\n");
    printf("BEST CHAIN SELECTED (Chain %d of %d)\n", best + 1, num_chains);
    printf("===========================================\n");
    printf("  Chain length: %d markers\n", winner->length);
    printf("  Score: %.1f / 100\n", winner->score);
    printf("  Sec 59 silent: %d/%d (%.0f%%)\n", 
           winner->sec59_silent_count, winner->length,
           100.0f * winner->sec59_silent_count / winner->length);
    printf("  Sec 29 silent: %d/%d (%.0f%%)\n",
           winner->sec29_silent_count, winner->length,
           100.0f * winner->sec29_silent_count / winner->length);
    printf("  Avg duration error: %.0f ms (from 800ms)\n", winner->avg_duration_error);
    printf("  Avg confidence: %.0f%%\n", winner->avg_confidence * 100);
    printf("\n");
    
    /* List the markers in the chain with refined timing */
    printf("  Chain members (with refined edge detection):\n");
    float refined_starts[MAX_CHAIN_LENGTH];
    float refined_ends[MAX_CHAIN_LENGTH];
    
    for (int i = 0; i < winner->length; i++) {
        marker_candidate_t *c = &candidates[winner->indices[i]];
        refined_starts[i] = refine_marker_start(c);
        refined_ends[i] = refine_marker_end(c);
        float refined_dur = refined_ends[i] - refined_starts[i];
        
        printf("    [%d] start=%.1fms, end=%.1fms, dur=%.0fms (was %dms), sec29=%s, sec59=%s\n",
               i + 1, refined_starts[i], refined_ends[i], refined_dur, c->duration_ms,
               c->sec29_silent ? "silent" : "TICK",
               c->sec59_silent ? "silent" : "TICK");
    }
    printf("\n");
    
    /* Compute timing using marker START (minute boundary is at start of 800ms pulse)
     * Average across all chain members with weighting by marker quality */
    printf("  TIMING CALCULATION:\n");
    printf("  Using marker START for timing (WWV minute starts at pulse onset)\n");
    
    float weighted_offset_sum = 0;
    float weight_sum = 0;
    
    for (int i = 0; i < winner->length; i++) {
        marker_candidate_t *c = &candidates[winner->indices[i]];
        float refined_start = refined_starts[i];
        
        /* This marker's offset: where minute boundary is relative to file start
         * Marker i occurs at minute (i) after first minute boundary
         * So: refined_start = first_minute_offset + i * 60000
         * Therefore: first_minute_offset = refined_start - i * 60000 */
        float this_offset = refined_start - (float)(i * 60000);
        
        /* Bring into range [0, 60000) */
        while (this_offset < 0) this_offset += 60000.0f;
        while (this_offset >= 60000) this_offset -= 60000.0f;
        
        /* Weight by marker quality: duration closeness to 800ms + sec59 silence */
        float dur_quality = 1.0f - fabsf(c->duration_ms - 800) / 400.0f;
        if (dur_quality < 0.1f) dur_quality = 0.1f;
        float silence_bonus = (c->sec59_silent ? 0.5f : 0) + (c->sec29_silent ? 0.25f : 0);
        float weight = dur_quality + silence_bonus;
        
        printf("    Marker %d: start=%.1fms, offset=%.1fms, weight=%.2f\n",
               i + 1, refined_start, this_offset, weight);
        
        weighted_offset_sum += this_offset * weight;
        weight_sum += weight;
    }
    
    float refined_offset_ms = weighted_offset_sum / weight_sum;
    printf("    Weighted average offset: %.1f ms\n\n", refined_offset_ms);
    
    /* Final result */
    printf("  ============================================\n");
    printf("  TIMING RESULT\n");
    printf("  ============================================\n");
    printf("  First minute boundary: %.3f sec into recording\n", 
           start_sec + refined_offset_ms / 1000.0);
    printf("  Recording started: %.3f sec after UTC minute\n",
           (60000.0 - refined_offset_ms) / 1000.0);
    printf("  Offset from file start to minute: %.1f ms\n", refined_offset_ms);
    printf("  ============================================\n");
    
    /* Quality assessment */
    if (winner->score >= 80 && winner->sec59_silent_count == winner->length) {
        printf("  Quality: EXCELLENT - High confidence lock\n");
    } else if (winner->score >= 60 && winner->sec59_silent_count >= winner->length / 2) {
        printf("  Quality: GOOD - Reliable timing\n");
    } else if (winner->score >= 40) {
        printf("  Quality: FAIR - Use with caution\n");
    } else {
        printf("  Quality: POOR - May be incorrect\n");
    }
    printf("\n");
    
    /* Decode BCD time from the marker with best duration (closest to 800ms) */
    int best_marker_idx = -1;
    int best_dur_error = 9999;
    for (int i = 0; i < winner->length; i++) {
        marker_candidate_t *c = &candidates[winner->indices[i]];
        /* Need at least 60 seconds of data after marker for BCD decode */
        if (c->start_ms + 60000 < g_env_count) {
            int dur_error = abs(c->duration_ms - 800);
            if (dur_error < best_dur_error) {
                best_dur_error = dur_error;
                best_marker_idx = i;
            }
        }
    }
    
    if (best_marker_idx >= 0) {
        marker_candidate_t *c = &candidates[winner->indices[best_marker_idx]];
        /* Use the refined start for better alignment */
        int refined_start_ms = (int)(refined_starts[best_marker_idx] + 0.5f);
        printf("\n  Using marker %d (dur=%dms, closest to 800ms) for BCD decode\n",
               best_marker_idx + 1, c->duration_ms);
        printf("  Refined start: %d ms (vs raw %d ms)\n", refined_start_ms, c->start_ms);
        decode_bcd_time(refined_start_ms, c->end_ms, start_sec);
    }
    
    return best;
}

static void print_usage(const char *prog) {
    printf("WWV Timing Synchronization Detector\n");
    printf("Usage: %s <file.iqr> [start_sec] [duration_sec] [options]\n", prog);
    printf("\nOptions:\n");
    printf("  -w          Use wideband envelope (no tone filter)\n");
    printf("  -g          Use Goertzel algorithm (more precise tone detection)\n");
    printf("  -f <freq>   Set tone frequency in Hz (default: 1000)\n");
    printf("              Use 600 for WWV 5MHz, 1000 for 10/15/20 MHz\n");
    printf("\nDetects minute markers from WWV recordings.\n");
    printf("Default: biquad bandpass at 1000Hz.\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
    
    const char *filename = NULL;
    double start_sec = -1.0;  /* -1 = not set */
    double duration_sec = 120.0;
    
    /* Parse arguments */
    int arg_idx = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0) {
            g_wideband = 1;
        } else if (strcmp(argv[i], "-g") == 0) {
            g_goertzel = 1;
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            g_tone_freq = (float)atof(argv[++i]);
            if (g_tone_freq < 100 || g_tone_freq > 5000) {
                fprintf(stderr, "Invalid frequency: %.0f Hz (must be 100-5000)\n", g_tone_freq);
                return 1;
            }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (!filename) {
            filename = argv[i];
        } else if (start_sec < 0 && atof(argv[i]) >= 0) {
            start_sec = atof(argv[i]);
        } else {
            duration_sec = atof(argv[i]);
        }
    }
    
    /* Default start_sec to 0 if not set */
    if (start_sec < 0) start_sec = 0.0;
    
    if (!filename) {
        print_usage(argv[0]);
        return 1;
    }
    
    iqr_reader_t *reader = NULL;
    iqr_error_t err;
    
    err = iqr_open(&reader, filename);
    if (err != IQR_OK) {
        fprintf(stderr, "Failed to open: %s\n", iqr_strerror(err));
        return 1;
    }
    
    const iqr_header_t *hdr = iqr_get_header(reader);
    float fs = (float)hdr->sample_rate_hz;
    double file_duration = (double)hdr->sample_count / fs;
    
    printf("===========================================\n");
    printf("WWV Sync Detector\n");
    printf("===========================================\n");
    printf("File: %s\n", filename);
    printf("Sample Rate: %.0f Hz\n", fs);
    printf("Center Freq: %u Hz\n", hdr->center_freq_hz);
    printf("Duration: %.2f sec\n", file_duration);
    printf("Mode: %s\n", g_wideband ? "WIDEBAND" : (g_goertzel ? "GOERTZEL" : "BIQUAD"));
    if (!g_wideband) {
        printf("Tone Freq: %.0f Hz\n", g_tone_freq);
    }
    printf("Analyzing: %.2f to %.2f sec\n", start_sec, start_sec + duration_sec);
    printf("\n");
    
    /* Pass 1: Extract envelope */
    printf("Pass 1: Extracting envelope...\n");
    if (extract_envelope(reader, start_sec, duration_sec) < 0) {
        iqr_close(reader);
        return 1;
    }
    printf("  %d ms of envelope data\n", g_env_count);
    
    /* Pass 2: Compute baseline and smooth */
    printf("Pass 2: Computing baseline (trailing window)...\n");
    compute_baseline();
    smooth_envelope();
    
    /* Pass 3: Compute adaptive thresholds */
    printf("Pass 3: Computing adaptive thresholds (mean + %.1f*sigma)...\n", SIGMA_THRESHOLD);
    compute_adaptive_threshold();
    
    /* Debug: print threshold stats */
    {
        float thresh_min = 999, thresh_max = 0, thresh_sum = 0;
        float std_min = 999, std_max = 0, std_sum = 0;
        for (int i = REF_WINDOW_MS + REF_GAP_MS; i < g_env_count; i++) {
            if (g_adaptive_thresh[i] < thresh_min) thresh_min = g_adaptive_thresh[i];
            if (g_adaptive_thresh[i] > thresh_max) thresh_max = g_adaptive_thresh[i];
            thresh_sum += g_adaptive_thresh[i];
            if (g_local_std[i] < std_min) std_min = g_local_std[i];
            if (g_local_std[i] > std_max) std_max = g_local_std[i];
            std_sum += g_local_std[i];
        }
        int n = g_env_count - REF_WINDOW_MS - REF_GAP_MS;
        printf("  Adaptive threshold: min=%.4f, max=%.4f, avg=%.4f\n", 
               thresh_min, thresh_max, thresh_sum / n);
        printf("  Local std dev: min=%.4f, max=%.4f, avg=%.4f\n",
               std_min, std_max, std_sum / n);
    }
    
    /* Pass 4: Find markers - try adaptive first, then fallback */
    printf("Pass 4: Finding minute markers...\n");
    
    marker_candidate_t candidates[MAX_CANDIDATES];
    int num_candidates = 0;
    
    /* Try adaptive detection first (best for weak/varying signals) */
    printf("  Trying ADAPTIVE detection (mean + %.1f*sigma)...\n", SIGMA_THRESHOLD);
    num_candidates = find_markers_adaptive(candidates, MAX_CANDIDATES);
    
    if (num_candidates == 0) {
        /* Try dropout detection */
        printf("  No adaptive rises found. Trying DROPOUT detection (ratio < %.2f)...\n", DROP_THRESHOLD);
        num_candidates = find_markers_dropout(candidates, MAX_CANDIDATES);
    }
    
    if (num_candidates == 0) {
        /* Try fixed rise detection as last resort */
        printf("  No dropouts found. Trying fixed RISE detection (ratio > %.2f)...\n", RISE_THRESHOLD);
        num_candidates = find_markers_rise(candidates, MAX_CANDIDATES);
    }
    
    printf("  Found %d candidate(s)\n\n", num_candidates);
    
    /* Verify each candidate */
    for (int i = 0; i < num_candidates; i++) {
        verify_marker(&candidates[i], start_sec);
    }
    
    /* Check for 60-second spacing (boosts confidence) */
    if (num_candidates > 1) {
        printf("Pass 5: Checking 60-second spacing...\n");
        check_60sec_spacing(candidates, num_candidates);
        
        /* Filter to only candidates with 60s partners */
        int filtered_count = filter_by_spacing(candidates, num_candidates);
        printf("  Filtered: %d -> %d candidates with 60s partners\n", 
               num_candidates, filtered_count);
        num_candidates = filtered_count;
        
        if (num_candidates == 0) {
            printf("\nNo candidates with proper 60-second spacing found.\n");
            printf("This may indicate:\n");
            printf("  - Signal too weak to detect minute markers\n");
            printf("  - Recording too short (need > 60 seconds)\n");
            printf("  - Interference masking the markers\n");
        }
    }
    
    /* Display results */
    for (int i = 0; i < num_candidates; i++) {
        marker_candidate_t *c = &candidates[i];
        double marker_time = start_sec + c->end_ms / 1000.0;
        
        printf("-------------------------------------------\n");
        printf("Candidate %d: Minute marker at t=%.3f sec\n", i + 1, marker_time);
        printf("  Mode: %s\n", c->mode == 0 ? "DROPOUT" : (c->mode == 1 ? "RISE" : "ADAPTIVE"));
        printf("  Start: %.3f sec, End: %.3f sec\n", 
               start_sec + c->start_ms / 1000.0, marker_time);
        printf("  Duration: %d ms (expected 800ms)\n", c->duration_ms);
        printf("  %s: %.2f%s\n", 
               c->mode == 0 ? "Min ratio" : (c->mode == 2 ? "Peak sigmas" : "Peak ratio"),
               c->peak_ratio,
               c->mode == 2 ? " σ above mean" : "");
        printf("\n");
        printf("  Results:\n");
        printf("    Ticks detected: %d (of ~57 expected)\n", c->tick_count);
        printf("    Avg tick/gap ratio: %.2f\n", c->avg_ratio);
        printf("    Second 29: %s\n", c->sec29_silent ? "SILENT (good)" : "HAS TICK (bad)");
        printf("    Second 59: %s\n", c->sec59_silent ? "SILENT (good)" : "HAS TICK (bad)");
        printf("    Confidence: %.0f%%\n", c->confidence * 100);
        printf("\n");
        
        if (c->confidence > 0.6f) {
            printf("  >>> PROBABLE LOCK <<<\n");
            printf("  Minute boundary: %.3f sec into file\n", marker_time);
        }
        if (c->confidence > 0.8f && c->sec29_silent && c->sec59_silent) {
            printf("  >>> LOCK CONFIRMED <<<\n");
        }
    }
    
    /* Pass 6: Build chains and select best */
    if (num_candidates >= 2) {
        printf("\nPass 6: Building marker chains...\n");
        
        #define MAX_CHAINS 16
        marker_chain_t chains[MAX_CHAINS];
        int num_chains = build_chains(candidates, num_candidates, chains, MAX_CHAINS);
        
        printf("  Found %d chain(s) with 60s spacing\n", num_chains);
        
        if (num_chains > 0) {
            /* Show all chains briefly */
            for (int i = 0; i < num_chains; i++) {
                score_chain(&chains[i], candidates);
                printf("  Chain %d: %d markers, score=%.1f, sec59_silent=%d/%d\n",
                       i + 1, chains[i].length, chains[i].score,
                       chains[i].sec59_silent_count, chains[i].length);
            }
            
            /* Select and display best chain */
            select_best_chain(chains, num_chains, candidates, start_sec);
        }
    }
    
    if (num_candidates == 0) {
        printf("No minute markers found.\n");
        printf("\nDebug: Envelope statistics:\n");
        float sum = 0, min = 999, max = 0;
        for (int i = 0; i < g_env_count; i++) {
            sum += g_envelope[i];
            if (g_envelope[i] < min) min = g_envelope[i];
            if (g_envelope[i] > max) max = g_envelope[i];
        }
        printf("  Min: %.4f, Max: %.4f, Avg: %.4f\n", min, max, sum / g_env_count);
        printf("  Max/Avg ratio: %.2f\n", max / (sum / g_env_count));
        
        printf("\nSuggestions:\n");
        printf("  - Try -w flag for wideband mode\n");
        printf("  - Check signal strength (try lower gain reduction)\n");
        printf("  - For 5 MHz, note that WWV uses 600 Hz tone (not 1000 Hz)\n");
        printf("  - Try longer recording duration\n");
    }
    
    printf("===========================================\n");
    
    /* Cleanup */
    free(g_envelope);
    free(g_baseline);
    free(g_smooth);
    free(g_local_mean);
    free(g_local_std);
    free(g_adaptive_thresh);
    iqr_close(reader);
    
    return 0;
}
