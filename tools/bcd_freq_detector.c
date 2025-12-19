/**
 * @file bcd_freq_detector.c
 * @brief WWV BCD frequency-domain detector implementation
 *
 * Self-contained module with:
 *   - Own 2048-point FFT (40.96ms frames for precise frequency isolation)
 *   - Sliding window accumulator
 *   - Self-tracking baseline
 *   - CSV logging
 *
 * Pattern: Follows marker_detector.c structure
 *
 * This detector provides confident 100Hz identification.
 * Works in parallel with bcd_time_detector which provides precise edge timing.
 * The bcd_correlator combines both for reliable symbol output.
 */

#include "bcd_freq_detector.h"
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

#define FRAME_DURATION_MS   ((float)BCD_FREQ_FFT_SIZE * 1000.0f / BCD_FREQ_SAMPLE_RATE)
#define HZ_PER_BIN          ((float)BCD_FREQ_SAMPLE_RATE / BCD_FREQ_FFT_SIZE)
#define WINDOW_FRAMES       ((int)(BCD_FREQ_WINDOW_MS / FRAME_DURATION_MS))

/* Detection timing */
#define BCD_FREQ_COOLDOWN_MS        500.0f  /* Cooldown between detections */
#define BCD_FREQ_MAX_DURATION_MS    2000.0f /* Max time in pulse before timeout */

/* Warmup */
#define BCD_FREQ_WARMUP_FRAMES      50      /* ~2 seconds warmup */
#define BCD_FREQ_WARMUP_ADAPT_RATE  0.02f
#define BCD_FREQ_MIN_STARTUP_MS     5000.0f /* No pulses in first 5 seconds */

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

struct bcd_freq_detector {
    /* FFT resources */
    kiss_fft_cfg fft_cfg;
    kiss_fft_cpx *fft_in;
    kiss_fft_cpx *fft_out;
    float *window_func;

    /* Sample buffer for FFT */
    float *i_buffer;
    float *q_buffer;
    int buffer_idx;

    /* Sliding window accumulator */
    float *energy_history;          /* Circular buffer of frame energies */
    int history_idx;
    int history_count;
    float accumulated_energy;       /* Sum of energy_history */
    float baseline_energy;          /* Self-tracked noise floor */

    /* Detection state */
    detector_state_t state;
    float current_energy;           /* Current frame's 100Hz bucket energy */
    float threshold;

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
    bcd_freq_callback_fn callback;
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
 * At 50kHz with 2048-pt FFT, each bin is ~24.4 Hz
 * 100 Hz is in bin ~4 (100/24.4 ≈ 4.1)
 */
static float calculate_bucket_energy(bcd_freq_detector_t *fd) {
    int center_bin = (int)(BCD_FREQ_TARGET_FREQ_HZ / HZ_PER_BIN + 0.5f);
    int bin_span = (int)(BCD_FREQ_BANDWIDTH_HZ / HZ_PER_BIN + 0.5f);
    if (bin_span < 1) bin_span = 1;

    float pos_energy = 0.0f;
    float neg_energy = 0.0f;

    for (int b = -bin_span; b <= bin_span; b++) {
        int pos_bin = center_bin + b;
        int neg_bin = BCD_FREQ_FFT_SIZE - center_bin + b;

        if (pos_bin >= 0 && pos_bin < BCD_FREQ_FFT_SIZE) {
            float re = fd->fft_out[pos_bin].r;
            float im = fd->fft_out[pos_bin].i;
            pos_energy += sqrtf(re * re + im * im) / BCD_FREQ_FFT_SIZE;
        }
        if (neg_bin >= 0 && neg_bin < BCD_FREQ_FFT_SIZE) {
            float re = fd->fft_out[neg_bin].r;
            float im = fd->fft_out[neg_bin].i;
            neg_energy += sqrtf(re * re + im * im) / BCD_FREQ_FFT_SIZE;
        }
    }

    return pos_energy + neg_energy;
}

/**
 * Get wall clock time string for CSV output
 */
static void get_wall_time_str(bcd_freq_detector_t *fd, float timestamp_ms, char *buf, size_t buflen) {
    time_t event_time = fd->start_time + (time_t)(timestamp_ms / 1000.0f);
    struct tm *tm_info = localtime(&event_time);
    strftime(buf, buflen, "%H:%M:%S", tm_info);
}

/**
 * Update sliding window accumulator
 */
static void update_accumulator(bcd_freq_detector_t *fd, float energy) {
    if (fd->history_count >= WINDOW_FRAMES) {
        fd->accumulated_energy -= fd->energy_history[fd->history_idx];
    }

    fd->energy_history[fd->history_idx] = energy;
    fd->accumulated_energy += energy;

    fd->history_idx = (fd->history_idx + 1) % WINDOW_FRAMES;
    if (fd->history_count < WINDOW_FRAMES) {
        fd->history_count++;
    }
}

static void run_state_machine(bcd_freq_detector_t *fd) {
    float energy = fd->current_energy;
    uint64_t frame = fd->frame_count;

    update_accumulator(fd, energy);

    /* Warmup phase - fast adaptation to learn baseline */
    if (!fd->warmup_complete) {
        fd->baseline_energy += BCD_FREQ_WARMUP_ADAPT_RATE * (fd->accumulated_energy - fd->baseline_energy);
        fd->threshold = fd->baseline_energy * BCD_FREQ_THRESHOLD_MULT;

        if (frame >= fd->start_frame + BCD_FREQ_WARMUP_FRAMES) {
            fd->warmup_complete = true;
            printf("[BCD_FREQ] Warmup complete. Baseline=%.4f, Thresh=%.4f, Accum=%.4f\n",
                   fd->baseline_energy, fd->threshold, fd->accumulated_energy);
        }
        return;
    }

    /* No pulses in first few seconds - baseline still stabilizing */
    float timestamp_ms = fd->frame_count * FRAME_DURATION_MS;
    if (timestamp_ms < BCD_FREQ_MIN_STARTUP_MS) {
        fd->baseline_energy += BCD_FREQ_NOISE_ADAPT_RATE * (fd->accumulated_energy - fd->baseline_energy);
        fd->threshold = fd->baseline_energy * BCD_FREQ_THRESHOLD_MULT;
        return;
    }

    /* Self-track baseline during IDLE */
    if (fd->state == STATE_IDLE) {
        fd->baseline_energy += BCD_FREQ_NOISE_ADAPT_RATE * (fd->accumulated_energy - fd->baseline_energy);
        if (fd->baseline_energy < 0.0001f) fd->baseline_energy = 0.0001f;
        fd->threshold = fd->baseline_energy * BCD_FREQ_THRESHOLD_MULT;
    }

    /* State machine */
    switch (fd->state) {
        case STATE_IDLE:
            if (fd->accumulated_energy > fd->threshold) {
                fd->state = STATE_IN_PULSE;
                fd->pulse_start_frame = frame;
                fd->pulse_peak_energy = fd->accumulated_energy;
                fd->pulse_duration_frames = 1;
                fd->consecutive_low_frames = 0;
            }
            break;

        case STATE_IN_PULSE:
            fd->pulse_duration_frames++;
            if (fd->accumulated_energy > fd->pulse_peak_energy) {
                fd->pulse_peak_energy = fd->accumulated_energy;
            }

            /* Check for timeout or signal drop */
            float duration_ms = fd->pulse_duration_frames * FRAME_DURATION_MS;
            bool timed_out = (duration_ms > BCD_FREQ_MAX_DURATION_MS);

            /* Phase 9: Require consecutive low frames before ending pulse */
            if (fd->accumulated_energy < fd->threshold) {
                fd->consecutive_low_frames++;
            } else {
                fd->consecutive_low_frames = 0;  /* Reset if signal returns */
            }

            if ((fd->consecutive_low_frames >= MIN_LOW_FRAMES) || timed_out) {
                float start_timestamp_ms = fd->pulse_start_frame * FRAME_DURATION_MS;

                if (duration_ms >= BCD_FREQ_PULSE_MIN_MS &&
                    duration_ms <= BCD_FREQ_PULSE_MAX_MS) {
                    /* Valid pulse! */
                    fd->pulses_detected++;

                    float snr_db = 10.0f * log10f(fd->pulse_peak_energy / fd->baseline_energy);

                    printf("[BCD_FREQ] Pulse #%d at %.1fms  dur=%.0fms  accum=%.4f  SNR=%.1fdB\n",
                           fd->pulses_detected, start_timestamp_ms, duration_ms,
                           fd->pulse_peak_energy, snr_db);

                    /* CSV logging and telemetry */
                    char time_str[16];
                    get_wall_time_str(fd, start_timestamp_ms, time_str, sizeof(time_str));

                    if (fd->csv_file) {
                        fprintf(fd->csv_file, "%s,%.1f,%d,%.6f,%.0f,%.6f,%.1f\n",
                                time_str, start_timestamp_ms, fd->pulses_detected,
                                fd->pulse_peak_energy, duration_ms,
                                fd->baseline_energy, snr_db);
                        fflush(fd->csv_file);
                    }

                    /* UDP telemetry */
                    telem_sendf(TELEM_BCDS, "FREQ,%s,%.1f,%d,%.6f,%.0f,%.6f,%.1f",
                                time_str, start_timestamp_ms, fd->pulses_detected,
                                fd->pulse_peak_energy, duration_ms,
                                fd->baseline_energy, snr_db);

                    fd->last_pulse_frame = fd->pulse_start_frame;

                    /* Callback */
                    if (fd->callback) {
                        bcd_freq_event_t event = {
                            .timestamp_ms = start_timestamp_ms,
                            .duration_ms = duration_ms,
                            .accumulated_energy = fd->pulse_peak_energy,
                            .baseline_energy = fd->baseline_energy,
                            .snr_db = snr_db
                        };
                        fd->callback(&event, fd->callback_user_data);
                    }
                } else if (timed_out) {
                    printf("[BCD_FREQ] Timeout after %.0fms - resetting baseline\n", duration_ms);
                    fd->baseline_energy = fd->accumulated_energy;
                    fd->threshold = fd->baseline_energy * BCD_FREQ_THRESHOLD_MULT;
                    fd->pulses_rejected++;
                } else {
                    fd->pulses_rejected++;
                }

                fd->state = STATE_COOLDOWN;
                fd->cooldown_frames = MS_TO_FRAMES(BCD_FREQ_COOLDOWN_MS);
            }
            break;

        case STATE_COOLDOWN:
            if (--fd->cooldown_frames <= 0) {
                fd->state = STATE_IDLE;
            }
            break;
    }
}

/*============================================================================
 * Public API Implementation
 *============================================================================*/

bcd_freq_detector_t *bcd_freq_detector_create(const char *csv_path) {
    bcd_freq_detector_t *fd = (bcd_freq_detector_t *)calloc(1, sizeof(bcd_freq_detector_t));
    if (!fd) return NULL;

    fd->fft_cfg = kiss_fft_alloc(BCD_FREQ_FFT_SIZE, 0, NULL, NULL);
    if (!fd->fft_cfg) {
        free(fd);
        return NULL;
    }

    fd->fft_in = (kiss_fft_cpx *)malloc(BCD_FREQ_FFT_SIZE * sizeof(kiss_fft_cpx));
    fd->fft_out = (kiss_fft_cpx *)malloc(BCD_FREQ_FFT_SIZE * sizeof(kiss_fft_cpx));
    fd->window_func = (float *)malloc(BCD_FREQ_FFT_SIZE * sizeof(float));
    fd->i_buffer = (float *)malloc(BCD_FREQ_FFT_SIZE * sizeof(float));
    fd->q_buffer = (float *)malloc(BCD_FREQ_FFT_SIZE * sizeof(float));
    fd->energy_history = (float *)malloc(WINDOW_FRAMES * sizeof(float));

    if (!fd->fft_in || !fd->fft_out || !fd->window_func ||
        !fd->i_buffer || !fd->q_buffer || !fd->energy_history) {
        bcd_freq_detector_destroy(fd);
        return NULL;
    }

    /* Hann window */
    for (int i = 0; i < BCD_FREQ_FFT_SIZE; i++) {
        fd->window_func[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (BCD_FREQ_FFT_SIZE - 1)));
    }

    memset(fd->i_buffer, 0, BCD_FREQ_FFT_SIZE * sizeof(float));
    memset(fd->q_buffer, 0, BCD_FREQ_FFT_SIZE * sizeof(float));
    fd->buffer_idx = 0;

    memset(fd->energy_history, 0, WINDOW_FRAMES * sizeof(float));
    fd->history_idx = 0;
    fd->history_count = 0;
    fd->accumulated_energy = 0.0f;
    fd->baseline_energy = 0.0001f;

    fd->state = STATE_IDLE;
    fd->threshold = fd->baseline_energy * BCD_FREQ_THRESHOLD_MULT;
    fd->detection_enabled = true;
    fd->warmup_complete = false;
    fd->start_time = time(NULL);

    if (csv_path) {
        fd->csv_file = fopen(csv_path, "w");
        if (fd->csv_file) {
            char time_str[64];
            time_t now = time(NULL);
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));
            fprintf(fd->csv_file, "# Phoenix SDR BCD Freq Detector Log v%s\n", PHOENIX_VERSION_FULL);
            fprintf(fd->csv_file, "# Started: %s\n", time_str);
            fprintf(fd->csv_file, "# FFT: %d (%.2fms), Window: %d frames (%.0fms)\n",
                    BCD_FREQ_FFT_SIZE, FRAME_DURATION_MS, WINDOW_FRAMES, BCD_FREQ_WINDOW_MS);
            fprintf(fd->csv_file, "# Target: %dHz ±%dHz\n",
                    BCD_FREQ_TARGET_FREQ_HZ, BCD_FREQ_BANDWIDTH_HZ);
            fprintf(fd->csv_file, "time,timestamp_ms,pulse_num,accum_energy,duration_ms,baseline,snr_db\n");
            fflush(fd->csv_file);
        }
    }

    printf("[BCD_FREQ] Detector created: FFT=%d (%.2fms), window=%d frames (%.0fms)\n",
           BCD_FREQ_FFT_SIZE, FRAME_DURATION_MS, WINDOW_FRAMES, BCD_FREQ_WINDOW_MS);
    printf("[BCD_FREQ] Target: %dHz ±%dHz, self-tracking baseline\n",
           BCD_FREQ_TARGET_FREQ_HZ, BCD_FREQ_BANDWIDTH_HZ);

    return fd;
}

void bcd_freq_detector_destroy(bcd_freq_detector_t *fd) {
    if (!fd) return;

    if (fd->csv_file) fclose(fd->csv_file);
    if (fd->fft_cfg) kiss_fft_free(fd->fft_cfg);
    free(fd->fft_in);
    free(fd->fft_out);
    free(fd->window_func);
    free(fd->i_buffer);
    free(fd->q_buffer);
    free(fd->energy_history);
    free(fd);
}

void bcd_freq_detector_set_callback(bcd_freq_detector_t *fd,
                                    bcd_freq_callback_fn callback,
                                    void *user_data) {
    if (!fd) return;
    fd->callback = callback;
    fd->callback_user_data = user_data;
}

bool bcd_freq_detector_process_sample(bcd_freq_detector_t *fd,
                                      float i_sample,
                                      float q_sample) {
    if (!fd || !fd->detection_enabled) return false;

    /* Buffer sample for FFT */
    fd->i_buffer[fd->buffer_idx] = i_sample;
    fd->q_buffer[fd->buffer_idx] = q_sample;
    fd->buffer_idx++;

    /* Not enough samples yet */
    if (fd->buffer_idx < BCD_FREQ_FFT_SIZE) {
        return false;
    }

    /* Buffer full - run FFT */
    fd->buffer_idx = 0;

    /* Apply window and load FFT input */
    for (int i = 0; i < BCD_FREQ_FFT_SIZE; i++) {
        fd->fft_in[i].r = fd->i_buffer[i] * fd->window_func[i];
        fd->fft_in[i].i = fd->q_buffer[i] * fd->window_func[i];
    }

    /* Run FFT */
    kiss_fft(fd->fft_cfg, fd->fft_in, fd->fft_out);

    /* Extract bucket energy */
    fd->current_energy = calculate_bucket_energy(fd);

    /* Run detection state machine */
    run_state_machine(fd);

    fd->frame_count++;

    return (fd->state == STATE_IN_PULSE && fd->pulse_duration_frames == 1);
}

void bcd_freq_detector_set_enabled(bcd_freq_detector_t *fd, bool enabled) {
    if (fd) fd->detection_enabled = enabled;
}

bool bcd_freq_detector_get_enabled(bcd_freq_detector_t *fd) {
    return fd ? fd->detection_enabled : false;
}

float bcd_freq_detector_get_accumulated_energy(bcd_freq_detector_t *fd) {
    return fd ? fd->accumulated_energy : 0.0f;
}

float bcd_freq_detector_get_baseline(bcd_freq_detector_t *fd) {
    return fd ? fd->baseline_energy : 0.0f;
}

float bcd_freq_detector_get_threshold(bcd_freq_detector_t *fd) {
    return fd ? fd->threshold : 0.0f;
}

float bcd_freq_detector_get_current_energy(bcd_freq_detector_t *fd) {
    return fd ? fd->current_energy : 0.0f;
}

int bcd_freq_detector_get_pulse_count(bcd_freq_detector_t *fd) {
    return fd ? fd->pulses_detected : 0;
}

void bcd_freq_detector_print_stats(bcd_freq_detector_t *fd) {
    if (!fd) return;

    float elapsed = fd->frame_count * FRAME_DURATION_MS / 1000.0f;

    printf("\n=== BCD FREQ DETECTOR STATS ===\n");
    printf("FFT: %d (%.2fms), Window: %d frames (%.0fms)\n",
           BCD_FREQ_FFT_SIZE, FRAME_DURATION_MS, WINDOW_FRAMES, BCD_FREQ_WINDOW_MS);
    printf("Target: %d Hz ±%d Hz\n", BCD_FREQ_TARGET_FREQ_HZ, BCD_FREQ_BANDWIDTH_HZ);
    printf("Elapsed: %.1fs  Detected: %d  Rejected: %d\n",
           elapsed, fd->pulses_detected, fd->pulses_rejected);
    printf("Baseline: %.6f  Threshold: %.6f  Accumulated: %.6f\n",
           fd->baseline_energy, fd->threshold, fd->accumulated_energy);
    printf("===============================\n");
}

float bcd_freq_detector_get_frame_duration_ms(void) {
    return FRAME_DURATION_MS;
}
