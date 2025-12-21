/**
 * @file marker_detector.c
 * @brief WWV minute marker detector implementation
 *
 * Detects 800ms pulses at 1000Hz using sliding window accumulator.
 *
 * Detection strategy:
 *   1. Extract 1000Hz bucket energy each FFT frame (5.12ms)
 *   2. Accumulate energy over sliding 1-second window (~195 frames)
 *   3. When accumulated energy exceeds 3x baseline, marker detected
 *   4. Require minimum duration (500ms) above threshold before triggering
 *
 * Baseline tracking:
 *   Uses self-tracking baseline that slowly adapts to accumulated energy
 *   during IDLE state. This was proven reliable in v133.
 *
 * IMPORTANT: This detector is self-contained. Do NOT inject external
 * baseline values from other FFT paths - they have incompatible scaling.
 * See 12/17/2025 analysis: external baselines caused 720x scale mismatch.
 */

#include "marker_detector.h"
#include "wwv_clock.h"
#include "kiss_fft.h"
#include "version.h"
#include "waterfall_telemetry.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/*============================================================================
 * Internal Configuration
 *============================================================================*/

#define FRAME_DURATION_MS   ((float)MARKER_FFT_SIZE * 1000.0f / MARKER_SAMPLE_RATE)
#define HZ_PER_BIN          ((float)MARKER_SAMPLE_RATE / MARKER_FFT_SIZE)

/* Detection thresholds - proven values from v133 */
#define MARKER_THRESHOLD_MULT       3.0f    /* Accumulated must be 3x baseline */
#define MARKER_NOISE_ADAPT_RATE     0.001f  /* Slow baseline adaptation */
#define MARKER_COOLDOWN_MS          30000.0f /* 30 sec between markers (they're 60 sec apart) */
#define MARKER_MAX_DURATION_MS      5000.0f /* Max time in IN_MARKER before forced exit */

/* Warmup */
#define MARKER_WARMUP_FRAMES        200     /* ~1 second warmup */
#define MARKER_WARMUP_ADAPT_RATE    0.02f   /* Faster adaptation during warmup */
#define MARKER_MIN_STARTUP_MS       10000.0f /* No markers in first 10 seconds */

/* Display */
#define MARKER_FLASH_FRAMES         30      /* UI flash duration */

#define MS_TO_FRAMES(ms)    ((int)((ms) / FRAME_DURATION_MS + 0.5f))

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/*============================================================================
 * Internal State
 *============================================================================*/

typedef enum {
    STATE_IDLE,
    STATE_IN_MARKER,
    STATE_COOLDOWN
} detector_state_t;

struct marker_detector {
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
    int history_idx;                /* Write position */
    int history_count;              /* Frames accumulated so far */
    float accumulated_energy;       /* Sum of energy_history */
    float baseline_energy;          /* Self-tracked noise floor */

    /* Detection state */
    detector_state_t state;
    float current_energy;           /* Current frame's 1000Hz bucket energy */
    float threshold;                /* Detection threshold */

    /* Marker measurement */
    uint64_t marker_start_frame;
    float marker_peak_energy;
    int marker_duration_frames;
    int cooldown_frames;

    /* Statistics */
    int markers_detected;
    uint64_t last_marker_frame;
    uint64_t frame_count;
    uint64_t start_frame;
    bool warmup_complete;

    /* UI feedback */
    int flash_frames_remaining;
    bool detection_enabled;

    /* Tunable parameters (runtime adjustable via UDP commands) */
    float threshold_multiplier;     /* Threshold above baseline (2.0-5.0, default 3.0) */
    float noise_adapt_rate;         /* Baseline adaptation rate (0.0001-0.01, default 0.001) */
    float min_duration_ms;          /* Minimum pulse duration (300.0-700.0, default 500.0) */

    /* Callback */
    marker_callback_fn callback;
    void *callback_user_data;

    /* Logging */
    FILE *csv_file;
    FILE *debug_file;
    time_t start_time;

    /* WWV clock for expected event lookup */
    wwv_clock_t *wwv_clock;
};

/*============================================================================
 * Internal Functions
 *============================================================================*/

static float calculate_bucket_energy(marker_detector_t *md) {
    int center_bin = (int)(MARKER_TARGET_FREQ_HZ / HZ_PER_BIN + 0.5f);
    int bin_span = (int)(MARKER_BANDWIDTH_HZ / HZ_PER_BIN + 0.5f);
    if (bin_span < 1) bin_span = 1;

    float pos_energy = 0.0f;
    float neg_energy = 0.0f;

    for (int b = -bin_span; b <= bin_span; b++) {
        int pos_bin = center_bin + b;
        int neg_bin = MARKER_FFT_SIZE - center_bin + b;

        if (pos_bin >= 0 && pos_bin < MARKER_FFT_SIZE) {
            float re = md->fft_out[pos_bin].r;
            float im = md->fft_out[pos_bin].i;
            pos_energy += sqrtf(re * re + im * im) / MARKER_FFT_SIZE;
        }
        if (neg_bin >= 0 && neg_bin < MARKER_FFT_SIZE) {
            float re = md->fft_out[neg_bin].r;
            float im = md->fft_out[neg_bin].i;
            neg_energy += sqrtf(re * re + im * im) / MARKER_FFT_SIZE;
        }
    }

    return pos_energy + neg_energy;
}

static void get_wall_time_str(marker_detector_t *md, float timestamp_ms, char *buf, size_t buflen) {
    time_t event_time = md->start_time + (time_t)(timestamp_ms / 1000.0f);
    struct tm *tm_info = localtime(&event_time);
    strftime(buf, buflen, "%H:%M:%S", tm_info);
}

static void update_accumulator(marker_detector_t *md, float energy) {
    if (md->history_count >= MARKER_WINDOW_FRAMES) {
        md->accumulated_energy -= md->energy_history[md->history_idx];
    }

    md->energy_history[md->history_idx] = energy;
    md->accumulated_energy += energy;

    md->history_idx = (md->history_idx + 1) % MARKER_WINDOW_FRAMES;
    if (md->history_count < MARKER_WINDOW_FRAMES) {
        md->history_count++;
    }
}

static void run_state_machine(marker_detector_t *md) {
    float energy = md->current_energy;
    uint64_t frame = md->frame_count;

    update_accumulator(md, energy);

    /* Debug logging - every 20th frame (~100ms) */
    if (md->debug_file && (frame % 20 == 0)) {
        char time_str[16];
        get_wall_time_str(md, frame * FRAME_DURATION_MS, time_str, sizeof(time_str));
        const char *state_names[] = {"IDLE", "IN_MARKER", "COOLDOWN"};
        float ratio = (md->baseline_energy > 0.001f) ? md->accumulated_energy / md->baseline_energy : 0.0f;
        fprintf(md->debug_file, "%s,%.1f,%s,%.1f,%.1f,%.1f,%.4f,%.2f\n",
                time_str, frame * FRAME_DURATION_MS, state_names[md->state],
                md->accumulated_energy, md->baseline_energy, md->threshold,
                energy, ratio);
        fflush(md->debug_file);
    }

    /* Warmup phase - fast adaptation to learn baseline */
    if (!md->warmup_complete) {
        md->baseline_energy += MARKER_WARMUP_ADAPT_RATE * (md->accumulated_energy - md->baseline_energy);
        md->threshold = md->baseline_energy * md->threshold_multiplier;

        if (frame >= md->start_frame + MARKER_WARMUP_FRAMES) {
            md->warmup_complete = true;
            printf("[MARKER] Warmup complete. Baseline=%.1f, Thresh=%.1f, Accum=%.1f\n",
                   md->baseline_energy, md->threshold, md->accumulated_energy);
        }
        return;
    }

    /* No markers in first few seconds - baseline still stabilizing */
    float timestamp_ms = md->frame_count * FRAME_DURATION_MS;
    if (timestamp_ms < MARKER_MIN_STARTUP_MS) {
        md->baseline_energy += md->noise_adapt_rate * (md->accumulated_energy - md->baseline_energy);
        md->threshold = md->baseline_energy * md->threshold_multiplier;
        return;
    }

    /* Self-track baseline during IDLE (proven approach from v133) */
    if (md->state == STATE_IDLE) {
        md->baseline_energy += md->noise_adapt_rate * (md->accumulated_energy - md->baseline_energy);
        if (md->baseline_energy < 0.001f) md->baseline_energy = 0.001f;
        md->threshold = md->baseline_energy * md->threshold_multiplier;
    }

    /* State machine */
    switch (md->state) {
        case STATE_IDLE:
            if (md->accumulated_energy > md->threshold) {
                md->state = STATE_IN_MARKER;
                md->marker_start_frame = frame;
                md->marker_peak_energy = md->accumulated_energy;
                md->marker_duration_frames = 1;
            }
            break;

        case STATE_IN_MARKER:
            md->marker_duration_frames++;
            if (md->accumulated_energy > md->marker_peak_energy) {
                md->marker_peak_energy = md->accumulated_energy;
            }

            /* Check for timeout */
            float duration_ms = md->marker_duration_frames * FRAME_DURATION_MS;
            bool timed_out = (duration_ms > MARKER_MAX_DURATION_MS);

            if (md->accumulated_energy < md->threshold || timed_out) {
                if (duration_ms >= md->min_duration_ms && duration_ms < MARKER_MAX_DURATION_MS) {
                    /* Valid marker! */
                    md->markers_detected++;
                    md->flash_frames_remaining = MARKER_FLASH_FRAMES;

                    float timestamp_ms = frame * FRAME_DURATION_MS;
                    float since_last = (md->last_marker_frame > 0) ?
                        (md->marker_start_frame - md->last_marker_frame) * FRAME_DURATION_MS / 1000.0f : 0.0f;

                    printf("[%7.1fs] *** MINUTE MARKER #%d ***  dur=%.0fms  since=%.1fs  accum=%.2f\n",
                           timestamp_ms / 1000.0f, md->markers_detected,
                           duration_ms, since_last, md->marker_peak_energy);

                    /* CSV logging and telemetry */
                    char time_str[16];
                    get_wall_time_str(md, timestamp_ms, time_str, sizeof(time_str));
                    wwv_time_t wwv = md->wwv_clock ? wwv_clock_now(md->wwv_clock) : (wwv_time_t){0};

                    if (md->csv_file) {
                        fprintf(md->csv_file, "%s,%.1f,M%d,%d,%s,%.6f,%.1f,%.1f,%.6f,%.6f\n",
                                time_str, timestamp_ms, md->markers_detected, wwv.second,
                                wwv_event_name(wwv.expected_event),
                                md->marker_peak_energy, duration_ms, since_last,
                                md->baseline_energy, md->threshold);
                        fflush(md->csv_file);
                    }

                    /* UDP telemetry */
                    telem_sendf(TELEM_MARKERS, "%s,%.1f,M%d,%d,%s,%.6f,%.1f,%.1f,%.6f,%.6f",
                                time_str, timestamp_ms, md->markers_detected, wwv.second,
                                wwv_event_name(wwv.expected_event),
                                md->marker_peak_energy, duration_ms, since_last,
                                md->baseline_energy, md->threshold);

                    if (md->callback) {
                        marker_event_t event = {
                            .marker_number = md->markers_detected,
                            .timestamp_ms = timestamp_ms,
                            .since_last_marker_sec = since_last,
                            .accumulated_energy = md->marker_peak_energy,
                            .peak_energy = md->current_energy,
                            .duration_ms = duration_ms
                        };
                        md->callback(&event, md->callback_user_data);
                    }

                    md->last_marker_frame = md->marker_start_frame;
                } else if (timed_out) {
                    printf("[MARKER] Timeout after %.0fms - resetting baseline\n", duration_ms);
                    md->baseline_energy = md->accumulated_energy;
                    md->threshold = md->baseline_energy * md->threshold_multiplier;
                }

                md->state = STATE_COOLDOWN;
                md->cooldown_frames = MS_TO_FRAMES(MARKER_COOLDOWN_MS);
            }
            break;

        case STATE_COOLDOWN:
            if (--md->cooldown_frames <= 0) {
                md->state = STATE_IDLE;
            }
            break;
    }
}

/*============================================================================
 * Public API Implementation
 *============================================================================*/

marker_detector_t *marker_detector_create(const char *csv_path) {
    marker_detector_t *md = (marker_detector_t *)calloc(1, sizeof(marker_detector_t));
    if (!md) return NULL;

    md->fft_cfg = kiss_fft_alloc(MARKER_FFT_SIZE, 0, NULL, NULL);
    if (!md->fft_cfg) {
        free(md);
        return NULL;
    }

    md->fft_in = (kiss_fft_cpx *)malloc(MARKER_FFT_SIZE * sizeof(kiss_fft_cpx));
    md->fft_out = (kiss_fft_cpx *)malloc(MARKER_FFT_SIZE * sizeof(kiss_fft_cpx));
    md->window_func = (float *)malloc(MARKER_FFT_SIZE * sizeof(float));
    md->i_buffer = (float *)malloc(MARKER_FFT_SIZE * sizeof(float));
    md->q_buffer = (float *)malloc(MARKER_FFT_SIZE * sizeof(float));
    md->energy_history = (float *)malloc(MARKER_WINDOW_FRAMES * sizeof(float));

    if (!md->fft_in || !md->fft_out || !md->window_func ||
        !md->i_buffer || !md->q_buffer || !md->energy_history) {
        marker_detector_destroy(md);
        return NULL;
    }

    /* Hann window */
    for (int i = 0; i < MARKER_FFT_SIZE; i++) {
        md->window_func[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (MARKER_FFT_SIZE - 1)));
    }

    memset(md->i_buffer, 0, MARKER_FFT_SIZE * sizeof(float));
    memset(md->q_buffer, 0, MARKER_FFT_SIZE * sizeof(float));
    md->buffer_idx = 0;

    memset(md->energy_history, 0, MARKER_WINDOW_FRAMES * sizeof(float));
    md->history_idx = 0;
    md->history_count = 0;
    md->accumulated_energy = 0.0f;
    md->baseline_energy = 0.01f;

    md->state = STATE_IDLE;
    md->threshold = md->baseline_energy * MARKER_THRESHOLD_MULT;
    md->detection_enabled = true;
    md->warmup_complete = false;
    md->start_time = time(NULL);

    /* Initialize tunable parameters to defaults */
    md->threshold_multiplier = MARKER_THRESHOLD_MULT;      /* 3.0 */
    md->noise_adapt_rate = MARKER_NOISE_ADAPT_RATE;        /* 0.001 */
    md->min_duration_ms = MARKER_MIN_DURATION_MS;          /* 500.0 */

    md->wwv_clock = wwv_clock_create(WWV_STATION_WWV);

    if (csv_path) {
        md->csv_file = fopen(csv_path, "w");
        if (md->csv_file) {
            char time_str[64];
            time_t now = time(NULL);
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));
            fprintf(md->csv_file, "# Phoenix SDR WWV Marker Log v%s\n", PHOENIX_VERSION_FULL);
            fprintf(md->csv_file, "# Started: %s\n", time_str);
            fprintf(md->csv_file, "# Sliding window: %d frames (%.0f ms)\n",
                    MARKER_WINDOW_FRAMES, MARKER_WINDOW_MS);
            fprintf(md->csv_file, "time,timestamp_ms,marker_num,wwv_sec,expected,accum_energy,duration_ms,since_last_sec,baseline,threshold\n");
            fflush(md->csv_file);
        }

        /* Debug log */
        char debug_path[512];
        strncpy(debug_path, csv_path, sizeof(debug_path) - 1);
        debug_path[sizeof(debug_path) - 1] = '\0';
        char *markers_pos = strstr(debug_path, "markers.csv");
        if (markers_pos) {
            strcpy(markers_pos, "debug_marker.csv");
        } else {
            strcat(debug_path, "_debug.csv");
        }
        md->debug_file = fopen(debug_path, "w");
        if (md->debug_file) {
            char time_str[64];
            time_t now = time(NULL);
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));
            fprintf(md->debug_file, "# Phoenix SDR Marker Debug Log v%s\n", PHOENIX_VERSION_FULL);
            fprintf(md->debug_file, "# Started: %s\n", time_str);
            fprintf(md->debug_file, "time,timestamp_ms,state,accum,baseline,threshold,energy,ratio\n");
            fflush(md->debug_file);
            printf("[MARKER] Debug log: %s\n", debug_path);
        }
    }

    printf("[MARKER] Detector created: FFT=%d (%.1fms), window=%d frames (%.0fms)\n",
           MARKER_FFT_SIZE, FRAME_DURATION_MS, MARKER_WINDOW_FRAMES, MARKER_WINDOW_MS);
    printf("[MARKER] Target: %dHz ±%dHz, self-tracking baseline\n",
           MARKER_TARGET_FREQ_HZ, MARKER_BANDWIDTH_HZ);

    return md;
}

void marker_detector_destroy(marker_detector_t *md) {
    if (!md) return;

    if (md->wwv_clock) wwv_clock_destroy(md->wwv_clock);
    if (md->csv_file) fclose(md->csv_file);
    if (md->debug_file) fclose(md->debug_file);
    if (md->fft_cfg) kiss_fft_free(md->fft_cfg);
    free(md->fft_in);
    free(md->fft_out);
    free(md->window_func);
    free(md->i_buffer);
    free(md->q_buffer);
    free(md->energy_history);
    free(md);
}

void marker_detector_set_callback(marker_detector_t *md, marker_callback_fn callback, void *user_data) {
    if (!md) return;
    md->callback = callback;
    md->callback_user_data = user_data;
}

bool marker_detector_process_sample(marker_detector_t *md, float i_sample, float q_sample) {
    if (!md || !md->detection_enabled) return false;

    md->i_buffer[md->buffer_idx] = i_sample;
    md->q_buffer[md->buffer_idx] = q_sample;
    md->buffer_idx++;

    if (md->buffer_idx < MARKER_FFT_SIZE) {
        return false;
    }

    md->buffer_idx = 0;

    for (int i = 0; i < MARKER_FFT_SIZE; i++) {
        md->fft_in[i].r = md->i_buffer[i] * md->window_func[i];
        md->fft_in[i].i = md->q_buffer[i] * md->window_func[i];
    }

    kiss_fft(md->fft_cfg, md->fft_in, md->fft_out);
    md->current_energy = calculate_bucket_energy(md);
    run_state_machine(md);
    md->frame_count++;

    return (md->flash_frames_remaining == MARKER_FLASH_FRAMES);
}

int marker_detector_get_flash_frames(marker_detector_t *md) {
    return md ? md->flash_frames_remaining : 0;
}

void marker_detector_decrement_flash(marker_detector_t *md) {
    if (md && md->flash_frames_remaining > 0) {
        md->flash_frames_remaining--;
    }
}

void marker_detector_set_enabled(marker_detector_t *md, bool enabled) {
    if (md) md->detection_enabled = enabled;
}

bool marker_detector_get_enabled(marker_detector_t *md) {
    return md ? md->detection_enabled : false;
}

float marker_detector_get_accumulated_energy(marker_detector_t *md) {
    return md ? md->accumulated_energy : 0.0f;
}

float marker_detector_get_threshold(marker_detector_t *md) {
    return md ? md->threshold : 0.0f;
}

float marker_detector_get_current_energy(marker_detector_t *md) {
    return md ? md->current_energy : 0.0f;
}

int marker_detector_get_marker_count(marker_detector_t *md) {
    return md ? md->markers_detected : 0;
}

void marker_detector_print_stats(marker_detector_t *md) {
    if (!md) return;

    float elapsed = md->frame_count * FRAME_DURATION_MS / 1000.0f;
    int expected_markers = (int)(elapsed / 60.0f);

    printf("\n=== MARKER DETECTOR STATS ===\n");
    printf("FFT: %d (%.1fms), Window: %d frames (%.0fms)\n",
           MARKER_FFT_SIZE, FRAME_DURATION_MS, MARKER_WINDOW_FRAMES, MARKER_WINDOW_MS);
    printf("Target: %d Hz +/-%d Hz\n", MARKER_TARGET_FREQ_HZ, MARKER_BANDWIDTH_HZ);
    printf("Elapsed: %.1fs  Detected: %d  Expected: ~%d\n",
           elapsed, md->markers_detected, expected_markers);
    printf("Baseline: %.4f  Threshold: %.4f\n", md->baseline_energy, md->threshold);
    printf("=============================\n");
}

void marker_detector_log_metadata(marker_detector_t *md, uint64_t center_freq,
                                  uint32_t sample_rate, uint32_t gain_reduction,
                                  uint32_t lna_state) {
    if (!md || !md->csv_file) return;

    char time_str[64];
    time_t now = time(NULL);
    strftime(time_str, sizeof(time_str), "%H:%M:%S", localtime(&now));

    float timestamp_ms = md->frame_count * FRAME_DURATION_MS;

    fprintf(md->csv_file, "%s,%.1f,META,0,freq=%llu rate=%u GR=%u LNA=%u,0,0,0,0,0\n",
            time_str, timestamp_ms,
            (unsigned long long)center_freq, sample_rate, gain_reduction, lna_state);
    fflush(md->csv_file);
}

void marker_detector_log_display_gain(marker_detector_t *md, float display_gain) {
    if (!md || !md->csv_file) return;

    char time_str[64];
    time_t now = time(NULL);
    strftime(time_str, sizeof(time_str), "%H:%M:%S", localtime(&now));

    float timestamp_ms = md->frame_count * FRAME_DURATION_MS;

    fprintf(md->csv_file, "%s,%.1f,GAIN,0,display_gain=%+.0fdB,0,0,0,0,0\n",
            time_str, timestamp_ms, display_gain);
    fflush(md->csv_file);
}

float marker_detector_get_frame_duration_ms(void) {
    return FRAME_DURATION_MS;
}

/*============================================================================
 * Runtime Parameter Tuning
 *============================================================================*/

void marker_detector_set_threshold_mult(marker_detector_t *md, float mult) {
    if (!md || mult < 2.0f || mult > 5.0f) return;
    md->threshold_multiplier = mult;
    md->threshold = md->baseline_energy * md->threshold_multiplier;
}

float marker_detector_get_threshold_mult(marker_detector_t *md) {
    return md ? md->threshold_multiplier : 3.0f;
}

void marker_detector_set_noise_adapt_rate(marker_detector_t *md, float rate) {
    if (!md || rate < 0.0001f || rate > 0.01f) return;
    md->noise_adapt_rate = rate;
}

float marker_detector_get_noise_adapt_rate(marker_detector_t *md) {
    return md ? md->noise_adapt_rate : 0.001f;
}

void marker_detector_set_min_duration_ms(marker_detector_t *md, float ms) {
    if (!md || ms < 300.0f || ms > 700.0f) return;
    md->min_duration_ms = ms;
}

float marker_detector_get_min_duration_ms(marker_detector_t *md) {
    return md ? md->min_duration_ms : 500.0f;
}
