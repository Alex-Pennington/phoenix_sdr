/**
 * @file bcd_time_detector.c
 * @brief WWV BCD time-domain detector implementation
 *
 * Self-contained module with:
 *   - Own 256-point FFT (5.12ms frames for precise edge detection)
 *   - Own sample buffer
 *   - Adaptive threshold state machine
 *   - CSV logging
 *
 * Pattern: Follows tick_detector.c structure
 *
 * This detector provides precise pulse edge timestamps for 100Hz BCD pulses.
 * Works in parallel with bcd_freq_detector which provides confident 100Hz
 * identification. The bcd_correlator combines both for reliable symbol output.
 */

#include "bcd_time_detector.h"
#include "waterfall_telemetry.h"
#include "kiss_fft.h"
#include "version.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/*============================================================================
 * Internal Configuration
 *============================================================================*/

#define FRAME_DURATION_MS   ((float)BCD_TIME_FFT_SIZE * 1000.0f / BCD_TIME_SAMPLE_RATE)
#define HZ_PER_BIN          ((float)BCD_TIME_SAMPLE_RATE / BCD_TIME_FFT_SIZE)

/* Detection timing */
#define BCD_TIME_COOLDOWN_MS        200.0f  /* Prevent retriggering */

/* Threshold adaptation */
#define BCD_TIME_NOISE_ADAPT_DOWN   0.002f  /* Fast attack when signal drops */
#define BCD_TIME_NOISE_ADAPT_UP     0.0002f /* Slow decay to prevent learning pulses */
#define NOISE_FLOOR_MIN             0.0001f
#define NOISE_FLOOR_MAX             5.0f
#define BCD_TIME_WARMUP_ADAPT_RATE  0.05f
#define BCD_TIME_WARMUP_FRAMES      50

#define MS_TO_FRAMES(ms)    ((int)((ms) / FRAME_DURATION_MS + 0.5f))

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/*============================================================================
 * Internal State
 *============================================================================*/

typedef enum {
    STATE_IDLE,
    STATE_IN_PULSE,
    STATE_COOLDOWN
} detector_state_t;

struct bcd_time_detector {
    /* FFT resources */
    kiss_fft_cfg fft_cfg;
    kiss_fft_cpx *fft_in;
    kiss_fft_cpx *fft_out;
    float *window_func;

    /* Sample buffer for FFT */
    float *i_buffer;
    float *q_buffer;
    int buffer_idx;

    /* Detection state */
    detector_state_t state;
    float noise_floor;
    float threshold_high;
    float threshold_low;
    float current_energy;

    /* Pulse measurement */
    uint64_t pulse_start_frame;
    float pulse_peak_energy;
    int pulse_duration_frames;
    int cooldown_frames;

    /* Phase 9: Minimum duration validation */
    int consecutive_low_frames;  /* Debounce pulse end */
    #define MIN_LOW_FRAMES 3         /* Require 3 frames below threshold */

    /* Statistics */
    int pulses_detected;
    int pulses_rejected;
    uint64_t last_pulse_frame;
    uint64_t frame_count;
    uint64_t start_frame;
    bool warmup_complete;

    /* Enabled flag */
    bool detection_enabled;

    /* Callback */
    bcd_time_callback_fn callback;
    void *callback_user_data;

    /* Logging */
    FILE *csv_file;
    time_t start_time;
};

/*============================================================================
 * Internal Functions
 *============================================================================*/

/**
 * Calculate energy in the 100Hz frequency bucket
 * At 50kHz with 256-pt FFT, each bin is ~195 Hz
 * Bin 0 = DC, Bin 1 = ~195 Hz
 * For 100 Hz, we need bin 0-1 area (coarse resolution)
 */
static float calculate_bucket_energy(bcd_time_detector_t *td) {
    int center_bin = (int)(BCD_TIME_TARGET_FREQ_HZ / HZ_PER_BIN + 0.5f);
    int bin_span = (int)(BCD_TIME_BANDWIDTH_HZ / HZ_PER_BIN + 0.5f);
    if (bin_span < 1) bin_span = 1;

    float pos_energy = 0.0f;
    float neg_energy = 0.0f;

    for (int b = -bin_span; b <= bin_span; b++) {
        int pos_bin = center_bin + b;
        int neg_bin = BCD_TIME_FFT_SIZE - center_bin + b;

        if (pos_bin >= 0 && pos_bin < BCD_TIME_FFT_SIZE) {
            float re = td->fft_out[pos_bin].r;
            float im = td->fft_out[pos_bin].i;
            pos_energy += sqrtf(re * re + im * im) / BCD_TIME_FFT_SIZE;
        }
        if (neg_bin >= 0 && neg_bin < BCD_TIME_FFT_SIZE) {
            float re = td->fft_out[neg_bin].r;
            float im = td->fft_out[neg_bin].i;
            neg_energy += sqrtf(re * re + im * im) / BCD_TIME_FFT_SIZE;
        }
    }

    return pos_energy + neg_energy;
}

/**
 * Get wall clock time string for CSV output
 */
static void get_wall_time_str(bcd_time_detector_t *td, float timestamp_ms, char *buf, size_t buflen) {
    time_t event_time = td->start_time + (time_t)(timestamp_ms / 1000.0f);
    struct tm *tm_info = localtime(&event_time);
    strftime(buf, buflen, "%H:%M:%S", tm_info);
}

static void run_state_machine(bcd_time_detector_t *td) {
    float energy = td->current_energy;
    uint64_t frame = td->frame_count;

    /* Warmup phase - fast adaptation to establish baseline */
    if (!td->warmup_complete) {
        td->noise_floor += BCD_TIME_WARMUP_ADAPT_RATE * (energy - td->noise_floor);
        if (td->noise_floor < NOISE_FLOOR_MIN) td->noise_floor = NOISE_FLOOR_MIN;
        td->threshold_high = td->noise_floor * BCD_TIME_THRESHOLD_MULT;
        td->threshold_low = td->threshold_high * BCD_TIME_HYSTERESIS_RATIO;

        if (frame >= td->start_frame + BCD_TIME_WARMUP_FRAMES) {
            td->warmup_complete = true;
            printf("[BCD_TIME] Warmup complete. Noise=%.6f, Thresh=%.6f\n",
                   td->noise_floor, td->threshold_high);
        }
        return;
    }

    /* Adaptive noise floor - asymmetric: fast down, slow up */
    if (td->state == STATE_IDLE && energy < td->threshold_high) {
        if (energy < td->noise_floor) {
            td->noise_floor += BCD_TIME_NOISE_ADAPT_DOWN * (energy - td->noise_floor);
        } else {
            td->noise_floor += BCD_TIME_NOISE_ADAPT_UP * (energy - td->noise_floor);
        }
        if (td->noise_floor < NOISE_FLOOR_MIN) td->noise_floor = NOISE_FLOOR_MIN;
        if (td->noise_floor > NOISE_FLOOR_MAX) td->noise_floor = NOISE_FLOOR_MAX;
        td->threshold_high = td->noise_floor * BCD_TIME_THRESHOLD_MULT;
        td->threshold_low = td->threshold_high * BCD_TIME_HYSTERESIS_RATIO;
    }

    /* State machine */
    switch (td->state) {
        case STATE_IDLE:
            if (energy > td->threshold_high) {
                td->state = STATE_IN_PULSE;
                td->pulse_start_frame = frame;
                td->pulse_peak_energy = energy;
                td->pulse_duration_frames = 1;
                td->consecutive_low_frames = 0;
            }
            break;

        case STATE_IN_PULSE:
            td->pulse_duration_frames++;
            if (energy > td->pulse_peak_energy) {
                td->pulse_peak_energy = energy;
            }

            /* Phase 9: Require consecutive low frames before ending pulse */
            if (energy < td->threshold_low) {
                td->consecutive_low_frames++;
            } else {
                td->consecutive_low_frames = 0;  /* Reset if signal returns */
            }

            if (td->consecutive_low_frames >= MIN_LOW_FRAMES) {
                /* Pulse ended - check validity */
                float duration_ms = td->pulse_duration_frames * FRAME_DURATION_MS;
                float timestamp_ms = td->pulse_start_frame * FRAME_DURATION_MS;
                float snr_db = 10.0f * log10f(td->pulse_peak_energy / td->noise_floor);

                if (duration_ms >= BCD_TIME_PULSE_MIN_MS &&
                    duration_ms <= BCD_TIME_PULSE_MAX_MS) {
                    /* Valid pulse! */
                    td->pulses_detected++;

                    printf("[BCD_TIME] Pulse #%d at %.1fms  dur=%.0fms  SNR=%.1fdB\n",
                           td->pulses_detected, timestamp_ms, duration_ms, snr_db);

                    /* CSV logging and telemetry */
                    char time_str[16];
                    get_wall_time_str(td, timestamp_ms, time_str, sizeof(time_str));

                    if (td->csv_file) {
                        fprintf(td->csv_file, "%s,%.1f,%d,%.6f,%.0f,%.6f,%.1f\n",
                                time_str, timestamp_ms, td->pulses_detected,
                                td->pulse_peak_energy, duration_ms,
                                td->noise_floor, snr_db);
                        fflush(td->csv_file);
                    }

                    /* UDP telemetry */
                    telem_sendf(TELEM_BCDS, "TIME,%s,%.1f,%d,%.6f,%.0f,%.6f,%.1f",
                                time_str, timestamp_ms, td->pulses_detected,
                                td->pulse_peak_energy, duration_ms,
                                td->noise_floor, snr_db);

                    td->last_pulse_frame = td->pulse_start_frame;

                    /* Callback */
                    if (td->callback) {
                        bcd_time_event_t event = {
                            .timestamp_ms = timestamp_ms,
                            .duration_ms = duration_ms,
                            .peak_energy = td->pulse_peak_energy,
                            .noise_floor = td->noise_floor,
                            .snr_db = snr_db
                        };
                        td->callback(&event, td->callback_user_data);
                    }
                } else {
                    /* Rejected pulse */
                    td->pulses_rejected++;
                    if (duration_ms < BCD_TIME_PULSE_MIN_MS) {
                        /* Too short - likely noise, don't log */
                    } else {
                        printf("[BCD_TIME] Rejected: dur=%.0fms (>%.0fms max)\n",
                               duration_ms, BCD_TIME_PULSE_MAX_MS);
                    }
                }

                td->state = STATE_COOLDOWN;
                td->cooldown_frames = MS_TO_FRAMES(BCD_TIME_COOLDOWN_MS);
            }
            break;

        case STATE_COOLDOWN:
            if (--td->cooldown_frames <= 0) {
                td->state = STATE_IDLE;
            }
            break;
    }
}

/*============================================================================
 * Public API Implementation
 *============================================================================*/

bcd_time_detector_t *bcd_time_detector_create(const char *csv_path) {
    bcd_time_detector_t *td = (bcd_time_detector_t *)calloc(1, sizeof(bcd_time_detector_t));
    if (!td) return NULL;

    /* Allocate FFT */
    td->fft_cfg = kiss_fft_alloc(BCD_TIME_FFT_SIZE, 0, NULL, NULL);
    if (!td->fft_cfg) {
        free(td);
        return NULL;
    }

    td->fft_in = (kiss_fft_cpx *)malloc(BCD_TIME_FFT_SIZE * sizeof(kiss_fft_cpx));
    td->fft_out = (kiss_fft_cpx *)malloc(BCD_TIME_FFT_SIZE * sizeof(kiss_fft_cpx));
    td->window_func = (float *)malloc(BCD_TIME_FFT_SIZE * sizeof(float));
    td->i_buffer = (float *)malloc(BCD_TIME_FFT_SIZE * sizeof(float));
    td->q_buffer = (float *)malloc(BCD_TIME_FFT_SIZE * sizeof(float));

    if (!td->fft_in || !td->fft_out || !td->window_func ||
        !td->i_buffer || !td->q_buffer) {
        bcd_time_detector_destroy(td);
        return NULL;
    }

    /* Initialize Hann window */
    for (int i = 0; i < BCD_TIME_FFT_SIZE; i++) {
        td->window_func[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (BCD_TIME_FFT_SIZE - 1)));
    }

    /* Initialize buffers */
    memset(td->i_buffer, 0, BCD_TIME_FFT_SIZE * sizeof(float));
    memset(td->q_buffer, 0, BCD_TIME_FFT_SIZE * sizeof(float));
    td->buffer_idx = 0;

    /* Initialize state */
    td->state = STATE_IDLE;
    td->noise_floor = 0.0001f;
    td->threshold_high = td->noise_floor * BCD_TIME_THRESHOLD_MULT;
    td->threshold_low = td->threshold_high * BCD_TIME_HYSTERESIS_RATIO;
    td->detection_enabled = true;
    td->warmup_complete = false;
    td->start_time = time(NULL);

    /* Open CSV file */
    if (csv_path) {
        td->csv_file = fopen(csv_path, "w");
        if (td->csv_file) {
            char time_str[64];
            time_t now = time(NULL);
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));
            fprintf(td->csv_file, "# Phoenix SDR BCD Time Detector Log v%s\n", PHOENIX_VERSION_FULL);
            fprintf(td->csv_file, "# Started: %s\n", time_str);
            fprintf(td->csv_file, "# FFT: %d (%.2fms), Target: %dHz ±%dHz\n",
                    BCD_TIME_FFT_SIZE, FRAME_DURATION_MS,
                    BCD_TIME_TARGET_FREQ_HZ, BCD_TIME_BANDWIDTH_HZ);
            fprintf(td->csv_file, "time,timestamp_ms,pulse_num,peak_energy,duration_ms,noise_floor,snr_db\n");
            fflush(td->csv_file);
        }
    }

    printf("[BCD_TIME] Detector created: FFT=%d (%.2fms), Target=%dHz ±%dHz\n",
           BCD_TIME_FFT_SIZE, FRAME_DURATION_MS,
           BCD_TIME_TARGET_FREQ_HZ, BCD_TIME_BANDWIDTH_HZ);

    return td;
}

void bcd_time_detector_destroy(bcd_time_detector_t *td) {
    if (!td) return;

    if (td->csv_file) fclose(td->csv_file);
    if (td->fft_cfg) kiss_fft_free(td->fft_cfg);
    free(td->fft_in);
    free(td->fft_out);
    free(td->window_func);
    free(td->i_buffer);
    free(td->q_buffer);
    free(td);
}

void bcd_time_detector_set_callback(bcd_time_detector_t *td,
                                    bcd_time_callback_fn callback,
                                    void *user_data) {
    if (!td) return;
    td->callback = callback;
    td->callback_user_data = user_data;
}

bool bcd_time_detector_process_sample(bcd_time_detector_t *td,
                                      float i_sample,
                                      float q_sample) {
    if (!td || !td->detection_enabled) return false;

    /* Buffer sample for FFT */
    td->i_buffer[td->buffer_idx] = i_sample;
    td->q_buffer[td->buffer_idx] = q_sample;
    td->buffer_idx++;

    /* Not enough samples yet */
    if (td->buffer_idx < BCD_TIME_FFT_SIZE) {
        return false;
    }

    /* Buffer full - run FFT */
    td->buffer_idx = 0;

    /* Apply window and load FFT input */
    for (int i = 0; i < BCD_TIME_FFT_SIZE; i++) {
        td->fft_in[i].r = td->i_buffer[i] * td->window_func[i];
        td->fft_in[i].i = td->q_buffer[i] * td->window_func[i];
    }

    /* Run FFT */
    kiss_fft(td->fft_cfg, td->fft_in, td->fft_out);

    /* Extract bucket energy */
    td->current_energy = calculate_bucket_energy(td);

    /* Run detection state machine */
    run_state_machine(td);

    td->frame_count++;

    return (td->state == STATE_IN_PULSE && td->pulse_duration_frames == 1);
}

void bcd_time_detector_set_enabled(bcd_time_detector_t *td, bool enabled) {
    if (td) td->detection_enabled = enabled;
}

bool bcd_time_detector_get_enabled(bcd_time_detector_t *td) {
    return td ? td->detection_enabled : false;
}

float bcd_time_detector_get_noise_floor(bcd_time_detector_t *td) {
    return td ? td->noise_floor : 0.0f;
}

float bcd_time_detector_get_threshold(bcd_time_detector_t *td) {
    return td ? td->threshold_high : 0.0f;
}

float bcd_time_detector_get_current_energy(bcd_time_detector_t *td) {
    return td ? td->current_energy : 0.0f;
}

int bcd_time_detector_get_pulse_count(bcd_time_detector_t *td) {
    return td ? td->pulses_detected : 0;
}

void bcd_time_detector_print_stats(bcd_time_detector_t *td) {
    if (!td) return;

    float elapsed = td->frame_count * FRAME_DURATION_MS / 1000.0f;

    printf("\n=== BCD TIME DETECTOR STATS ===\n");
    printf("FFT: %d (%.2fms), Target: %d Hz ±%d Hz\n",
           BCD_TIME_FFT_SIZE, FRAME_DURATION_MS,
           BCD_TIME_TARGET_FREQ_HZ, BCD_TIME_BANDWIDTH_HZ);
    printf("Elapsed: %.1fs  Detected: %d  Rejected: %d\n",
           elapsed, td->pulses_detected, td->pulses_rejected);
    printf("Noise floor: %.6f  Threshold: %.6f\n",
           td->noise_floor, td->threshold_high);
    printf("===============================\n");
}

float bcd_time_detector_get_frame_duration_ms(void) {
    return FRAME_DURATION_MS;
}
