/**
 * @file tick_detector.c
 * @brief WWV tick pulse detector implementation
 *
 * Self-contained module with:
 *   - Own 256-point FFT (5.3ms frames for 5ms pulse detection)
 *   - Own sample buffer
 *   - Adaptive threshold state machine
 *   - CSV logging
 *
 * This is the pattern for all detection channels:
 *   1. Size FFT for target signal timing
 *   2. Buffer samples until FFT ready
 *   3. Extract energy in target frequency bucket
 *   4. Run detection state machine
 *   5. Report events via callback
 */

#include "tick_detector.h"
#include "wwv_clock.h"
#include "kiss_fft.h"
#include "version.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/*============================================================================
 * Internal Configuration
 *============================================================================*/

#define FRAME_DURATION_MS   ((float)TICK_FFT_SIZE * 1000.0f / TICK_SAMPLE_RATE)
#define HZ_PER_BIN          ((float)TICK_SAMPLE_RATE / TICK_FFT_SIZE)

/* Detection timing */
#define TICK_MIN_DURATION_MS    2.0f
#define TICK_MAX_DURATION_MS    50.0f
#define MARKER_MAX_DURATION_MS  1000.0f  /* Allow up to 1s for minute markers */
#define TICK_COOLDOWN_MS        500.0f
#define TICK_MIN_INTERVAL_MS    800.0f   /* Reject ticks closer than this */

/* Threshold adaptation */
#define TICK_NOISE_ADAPT_DOWN   0.002f   /* Fast attack when signal drops */
#define TICK_NOISE_ADAPT_UP     0.0002f  /* Slow decay to prevent learning ticks */
#define NOISE_FLOOR_MAX         5.0f
#define TICK_WARMUP_ADAPT_RATE  0.05f
#define TICK_HYSTERESIS_RATIO   0.7f
#define TICK_THRESHOLD_MULT     2.0f

/* Correlation thresholds */
#define CORR_THRESHOLD_MULT     3.0f    /* Correlation must be 3x noise floor */
#define CORR_NOISE_ADAPT        0.01f   /* Noise floor adaptation rate */
#define CORR_DECIMATION         8       /* Compute correlation every N samples */
#define MARKER_CORR_RATIO       80.0f   /* Corr ratio above this = minute marker (was 40, too low) */
#define MARKER_MIN_DURATION_MS  500.0f  /* Marker must be at least 500ms (was 100ms) */
#define MARKER_MAX_DURATION_MS_CHECK 900.0f  /* Marker should be under 900ms */

/* Warmup and display */
#define TICK_WARMUP_FRAMES      50
#define TICK_FLASH_FRAMES       5

/* History for averaging */
#define TICK_HISTORY_SIZE       30
#define TICK_AVG_WINDOW_MS      15000.0f

#define MS_TO_FRAMES(ms)    ((int)((ms) / FRAME_DURATION_MS + 0.5f))

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/*============================================================================
 * Internal State
 *============================================================================*/

typedef enum {
    STATE_IDLE,
    STATE_IN_TICK,
    STATE_COOLDOWN
} detector_state_t;

struct tick_detector {
    /* FFT resources */
    kiss_fft_cfg fft_cfg;
    kiss_fft_cpx *fft_in;
    kiss_fft_cpx *fft_out;
    float *window_func;

    /* Sample buffer for FFT */
    float *i_buffer;
    float *q_buffer;
    int buffer_idx;

    /* Matched filter resources */
    float *template_i;          /* Cosine template */
    float *template_q;          /* Sine template */
    float *corr_buf_i;          /* Circular buffer for correlation */
    float *corr_buf_q;
    int corr_buf_idx;           /* Write position in circular buffer */
    int corr_sample_count;      /* Total samples received */
    float corr_peak;            /* Peak correlation value this detection */
    float corr_sum;             /* Accumulated correlation during pulse */
    int corr_sum_count;         /* Number of correlation samples accumulated */
    int corr_peak_offset;       /* Sample offset of peak */
    float corr_noise_floor;     /* Correlation noise floor estimate */

    /* Detection state */
    detector_state_t state;
    float noise_floor;
    float threshold_high;
    float threshold_low;
    float current_energy;

    /* Tick measurement */
    uint64_t tick_start_frame;
    float tick_peak_energy;
    int tick_duration_frames;
    int cooldown_frames;

    /* Statistics */
    int ticks_detected;
    int ticks_rejected;
    int markers_detected;       /* Position/minute markers (long pulses) */
    uint64_t last_tick_frame;
    uint64_t last_marker_frame;
    uint64_t frame_count;
    uint64_t start_frame;
    bool warmup_complete;

    /* History for interval averaging */
    float tick_timestamps_ms[TICK_HISTORY_SIZE];
    int tick_history_idx;
    int tick_history_count;

    /* UI feedback */
    int flash_frames_remaining;
    bool detection_enabled;

    /* Callback */
    tick_callback_fn callback;
    void *callback_user_data;

    /* Logging */
    FILE *csv_file;
    time_t start_time;          /* Wall clock time when detector started */

    /* WWV broadcast clock */
    wwv_clock_t *wwv_clock;
};

/*============================================================================
 * Internal Functions
 *============================================================================*/

/**
 * Generate matched filter template: windowed 1000Hz tone
 */
static void generate_template(tick_detector_t *td) {
    for (int i = 0; i < TICK_TEMPLATE_SAMPLES; i++) {
        float t = (float)i / TICK_SAMPLE_RATE;
        /* Hann window for smooth edges */
        float window = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (TICK_TEMPLATE_SAMPLES - 1)));
        /* Complex tone at target frequency */
        td->template_i[i] = cosf(2.0f * M_PI * TICK_TARGET_FREQ_HZ * t) * window;
        td->template_q[i] = sinf(2.0f * M_PI * TICK_TARGET_FREQ_HZ * t) * window;
    }
}

/**
 * Compute correlation magnitude at current buffer position
 * Returns magnitude of complex correlation
 */
static float compute_correlation(tick_detector_t *td) {
    float sum_i = 0.0f;
    float sum_q = 0.0f;

    /* Correlate template with buffer (complex multiply and accumulate) */
    for (int i = 0; i < TICK_TEMPLATE_SAMPLES; i++) {
        /* Index into circular buffer, starting from oldest sample */
        int buf_idx = (td->corr_buf_idx - TICK_TEMPLATE_SAMPLES + i + TICK_CORR_BUFFER_SIZE) % TICK_CORR_BUFFER_SIZE;

        float sig_i = td->corr_buf_i[buf_idx];
        float sig_q = td->corr_buf_q[buf_idx];
        float tpl_i = td->template_i[i];
        float tpl_q = td->template_q[i];

        /* Complex multiply: (sig_i + j*sig_q) * (tpl_i - j*tpl_q) */
        sum_i += sig_i * tpl_i + sig_q * tpl_q;
        sum_q += sig_q * tpl_i - sig_i * tpl_q;
    }

    return sqrtf(sum_i * sum_i + sum_q * sum_q);
}

static float calculate_bucket_energy(tick_detector_t *td) {
    int center_bin = (int)(TICK_TARGET_FREQ_HZ / HZ_PER_BIN + 0.5f);
    int bin_span = (int)(TICK_BANDWIDTH_HZ / HZ_PER_BIN + 0.5f);
    if (bin_span < 1) bin_span = 1;

    float pos_energy = 0.0f;
    float neg_energy = 0.0f;

    for (int b = -bin_span; b <= bin_span; b++) {
        int pos_bin = center_bin + b;
        int neg_bin = TICK_FFT_SIZE - center_bin + b;

        if (pos_bin >= 0 && pos_bin < TICK_FFT_SIZE) {
            float re = td->fft_out[pos_bin].r;
            float im = td->fft_out[pos_bin].i;
            pos_energy += sqrtf(re * re + im * im) / TICK_FFT_SIZE;
        }
        if (neg_bin >= 0 && neg_bin < TICK_FFT_SIZE) {
            float re = td->fft_out[neg_bin].r;
            float im = td->fft_out[neg_bin].i;
            neg_energy += sqrtf(re * re + im * im) / TICK_FFT_SIZE;
        }
    }

    return pos_energy + neg_energy;
}

static float calculate_avg_interval(tick_detector_t *td, float current_time_ms) {
    if (td->tick_history_count < 2) return 0.0f;

    float cutoff = current_time_ms - TICK_AVG_WINDOW_MS;
    float sum = 0.0f;
    int count = 0;
    float prev_time = -1.0f;

    for (int i = 0; i < td->tick_history_count; i++) {
        int idx = (td->tick_history_idx - td->tick_history_count + i + TICK_HISTORY_SIZE) % TICK_HISTORY_SIZE;
        float t = td->tick_timestamps_ms[idx];
        if (t >= cutoff) {
            if (prev_time >= 0.0f) {
                sum += (t - prev_time);
                count++;
            }
            prev_time = t;
        }
    }

    return (count > 0) ? (sum / count) : 0.0f;
}

/**
 * Get wall clock time string for CSV output
 * Format: HH:MM:SS
 */
static void get_wall_time_str(tick_detector_t *td, float timestamp_ms, char *buf, size_t buflen) {
    time_t event_time = td->start_time + (time_t)(timestamp_ms / 1000.0f);
    struct tm *tm_info = localtime(&event_time);
    strftime(buf, buflen, "%H:%M:%S", tm_info);
}

static void run_state_machine(tick_detector_t *td) {
    float energy = td->current_energy;
    uint64_t frame = td->frame_count;

    /* Warmup phase - fast adaptation to establish baseline */
    if (!td->warmup_complete) {
        td->noise_floor += TICK_WARMUP_ADAPT_RATE * (energy - td->noise_floor);
        if (td->noise_floor < 0.0001f) td->noise_floor = 0.0001f;
        td->threshold_high = td->noise_floor * TICK_THRESHOLD_MULT;
        td->threshold_low = td->threshold_high * TICK_HYSTERESIS_RATIO;

        if (frame >= td->start_frame + TICK_WARMUP_FRAMES) {
            td->warmup_complete = true;
            printf("[TICK] Warmup complete. Noise=%.4f, Thresh=%.4f\n",
                   td->noise_floor, td->threshold_high);
        }
        return;
    }

    /* Adaptive noise floor - asymmetric: fast down, slow up */
    if (td->state == STATE_IDLE && energy < td->threshold_high) {
        if (energy < td->noise_floor) {
            td->noise_floor += TICK_NOISE_ADAPT_DOWN * (energy - td->noise_floor);
        } else {
            td->noise_floor += TICK_NOISE_ADAPT_UP * (energy - td->noise_floor);
        }
        if (td->noise_floor < 0.0001f) td->noise_floor = 0.0001f;
        if (td->noise_floor > NOISE_FLOOR_MAX) td->noise_floor = NOISE_FLOOR_MAX;
        td->threshold_high = td->noise_floor * TICK_THRESHOLD_MULT;
        td->threshold_low = td->threshold_high * TICK_HYSTERESIS_RATIO;
    }

    /* State machine */
    switch (td->state) {
        case STATE_IDLE:
            if (energy > td->threshold_high) {
                td->state = STATE_IN_TICK;
                td->tick_start_frame = frame;
                td->tick_peak_energy = energy;
                td->tick_duration_frames = 1;
                td->corr_peak = 0.0f;  /* Reset correlation peak for new detection */
                td->corr_sum = 0.0f;   /* Reset accumulated correlation */
                td->corr_sum_count = 0;
            }
            break;

        case STATE_IN_TICK:
            td->tick_duration_frames++;
            if (energy > td->tick_peak_energy) {
                td->tick_peak_energy = energy;
            }

            if (energy < td->threshold_low) {
                /* Signal dropped - classify based on duration */
                float duration_ms = td->tick_duration_frames * FRAME_DURATION_MS;
                float interval_ms = (td->last_tick_frame > 0) ?
                    (td->tick_start_frame - td->last_tick_frame) * FRAME_DURATION_MS : TICK_MIN_INTERVAL_MS + 1.0f;
                float timestamp_ms = frame * FRAME_DURATION_MS;
                float corr_ratio = (td->corr_noise_floor > 0.001f) ? 
                    td->corr_peak / td->corr_noise_floor : 0.0f;

                bool valid_interval = (interval_ms >= TICK_MIN_INTERVAL_MS);
                bool valid_correlation = (td->corr_peak > td->corr_noise_floor * CORR_THRESHOLD_MULT);

                /* Check for minute marker first (500-900ms, high correlation) */
                bool is_marker_duration = (duration_ms >= MARKER_MIN_DURATION_MS && 
                                           duration_ms <= MARKER_MAX_DURATION_MS_CHECK);
                bool is_marker_corr = (corr_ratio > MARKER_CORR_RATIO);

                if (is_marker_duration && valid_interval) {
                    /* MINUTE MARKER detected! */
                    td->markers_detected++;
                    td->flash_frames_remaining = TICK_FLASH_FRAMES * 6;  /* Long flash for marker */

                    float since_last_marker = (td->last_marker_frame > 0) ?
                        (td->tick_start_frame - td->last_marker_frame) * FRAME_DURATION_MS / 1000.0f : 0.0f;

                    printf("[%7.1fs] *** MINUTE MARKER #%-3d ***  dur=%.0fms  corr=%.1f  since=%.1fs\n",
                           timestamp_ms / 1000.0f, td->markers_detected,
                           duration_ms, corr_ratio, since_last_marker);

                    /* CSV logging */
                    if (td->csv_file) {
                        char time_str[16];
                        get_wall_time_str(td, timestamp_ms, time_str, sizeof(time_str));
                        wwv_time_t wwv = td->wwv_clock ? wwv_clock_now(td->wwv_clock) : (wwv_time_t){0};
                        fprintf(td->csv_file, "%s,%.1f,M%d,%d,%s,%.6f,%.1f,%.0f,%.0f,%.6f,%.2f,%.1f\n",
                                time_str, timestamp_ms, td->markers_detected, wwv.second,
                                wwv_event_name(wwv.expected_event),
                                td->tick_peak_energy, duration_ms, interval_ms, 0.0f,
                                td->noise_floor, td->corr_peak, corr_ratio);
                        fflush(td->csv_file);
                    }

                    td->last_marker_frame = td->tick_start_frame;
                    /* Don't update last_tick_frame - marker shouldn't affect tick timing */
                    
                } else if (duration_ms >= TICK_MIN_DURATION_MS && 
                           duration_ms <= TICK_MAX_DURATION_MS &&
                           valid_interval && valid_correlation) {
                    /* Normal tick */
                    td->ticks_detected++;
                    td->flash_frames_remaining = TICK_FLASH_FRAMES;

                    float avg_interval_ms = calculate_avg_interval(td, timestamp_ms);

                    /* Update history */
                    td->tick_timestamps_ms[td->tick_history_idx] = timestamp_ms;
                    td->tick_history_idx = (td->tick_history_idx + 1) % TICK_HISTORY_SIZE;
                    if (td->tick_history_count < TICK_HISTORY_SIZE) {
                        td->tick_history_count++;
                    }

                    /* Console output */
                    char indicator = (interval_ms > 950.0f && interval_ms < 1050.0f) ? ' ' : '!';
                    printf("[%7.1fs] TICK #%-4d  int=%6.0fms  avg=%6.0fms  corr=%.1f %c\n",
                           timestamp_ms / 1000.0f, td->ticks_detected,
                           interval_ms, avg_interval_ms, corr_ratio, indicator);

                    /* CSV logging */
                    if (td->csv_file) {
                        char time_str[16];
                        get_wall_time_str(td, timestamp_ms, time_str, sizeof(time_str));
                        wwv_time_t wwv = td->wwv_clock ? wwv_clock_now(td->wwv_clock) : (wwv_time_t){0};
                        fprintf(td->csv_file, "%s,%.1f,%d,%d,%s,%.6f,%.1f,%.0f,%.0f,%.6f,%.2f,%.1f\n",
                                time_str, timestamp_ms, td->ticks_detected, wwv.second,
                                wwv_event_name(wwv.expected_event),
                                td->tick_peak_energy, duration_ms, interval_ms, avg_interval_ms,
                                td->noise_floor, td->corr_peak, corr_ratio);
                        fflush(td->csv_file);
                    }

                    td->last_tick_frame = td->tick_start_frame;

                    /* Callback */
                    if (td->callback) {
                        tick_event_t event = {
                            .tick_number = td->ticks_detected,
                            .timestamp_ms = timestamp_ms,
                            .interval_ms = interval_ms,
                            .duration_ms = duration_ms,
                            .peak_energy = td->tick_peak_energy,
                            .avg_interval_ms = avg_interval_ms,
                            .noise_floor = td->noise_floor
                        };
                        td->callback(&event, td->callback_user_data);
                    }
                } else {
                    /* Rejected - duration in the gap zone (50-500ms) or failed other checks */
                    td->ticks_rejected++;
                    if (duration_ms > TICK_MAX_DURATION_MS) {
                        printf("[%7.1fs] REJECTED: dur=%.0fms (gap zone), corr=%.1f\n",
                               timestamp_ms / 1000.0f, duration_ms, corr_ratio);
                    }
                }

                td->state = STATE_COOLDOWN;
                td->cooldown_frames = MS_TO_FRAMES(TICK_COOLDOWN_MS);
                
            } else if (td->tick_duration_frames * FRAME_DURATION_MS > MARKER_MAX_DURATION_MS) {
                /* Pulse WAY too long (>1s) - something is wrong, bail out */
                td->ticks_rejected++;
                printf("[%7.1fs] REJECTED: pulse >1s, bailing out\n",
                       frame * FRAME_DURATION_MS / 1000.0f);
                td->state = STATE_COOLDOWN;
                td->cooldown_frames = MS_TO_FRAMES(TICK_COOLDOWN_MS);
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

tick_detector_t *tick_detector_create(const char *csv_path) {
    tick_detector_t *td = (tick_detector_t *)calloc(1, sizeof(tick_detector_t));
    if (!td) return NULL;

    /* Allocate FFT */
    td->fft_cfg = kiss_fft_alloc(TICK_FFT_SIZE, 0, NULL, NULL);
    if (!td->fft_cfg) {
        free(td);
        return NULL;
    }

    td->fft_in = (kiss_fft_cpx *)malloc(TICK_FFT_SIZE * sizeof(kiss_fft_cpx));
    td->fft_out = (kiss_fft_cpx *)malloc(TICK_FFT_SIZE * sizeof(kiss_fft_cpx));
    td->window_func = (float *)malloc(TICK_FFT_SIZE * sizeof(float));
    td->i_buffer = (float *)malloc(TICK_FFT_SIZE * sizeof(float));
    td->q_buffer = (float *)malloc(TICK_FFT_SIZE * sizeof(float));

    /* Allocate matched filter resources */
    td->template_i = (float *)malloc(TICK_TEMPLATE_SAMPLES * sizeof(float));
    td->template_q = (float *)malloc(TICK_TEMPLATE_SAMPLES * sizeof(float));
    td->corr_buf_i = (float *)malloc(TICK_CORR_BUFFER_SIZE * sizeof(float));
    td->corr_buf_q = (float *)malloc(TICK_CORR_BUFFER_SIZE * sizeof(float));

    if (!td->fft_in || !td->fft_out || !td->window_func || !td->i_buffer || !td->q_buffer ||
        !td->template_i || !td->template_q || !td->corr_buf_i || !td->corr_buf_q) {
        tick_detector_destroy(td);
        return NULL;
    }

    /* Initialize window function (Hann) */
    for (int i = 0; i < TICK_FFT_SIZE; i++) {
        td->window_func[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (TICK_FFT_SIZE - 1)));
    }

    /* Initialize buffers */
    memset(td->i_buffer, 0, TICK_FFT_SIZE * sizeof(float));
    memset(td->q_buffer, 0, TICK_FFT_SIZE * sizeof(float));
    td->buffer_idx = 0;

    /* Initialize matched filter */
    generate_template(td);
    memset(td->corr_buf_i, 0, TICK_CORR_BUFFER_SIZE * sizeof(float));
    memset(td->corr_buf_q, 0, TICK_CORR_BUFFER_SIZE * sizeof(float));
    td->corr_buf_idx = 0;
    td->corr_sample_count = 0;
    td->corr_noise_floor = 0.0f;

    /* Initialize state */
    td->state = STATE_IDLE;
    td->noise_floor = 0.01f;
    td->threshold_high = td->noise_floor * TICK_THRESHOLD_MULT;
    td->threshold_low = td->threshold_high * TICK_HYSTERESIS_RATIO;
    td->detection_enabled = true;
    td->warmup_complete = false;
    td->start_time = time(NULL);  /* Record wall clock start time */

    /* Create WWV clock tracker */
    td->wwv_clock = wwv_clock_create(WWV_STATION_WWV);

    /* Open CSV file */
    if (csv_path) {
        td->csv_file = fopen(csv_path, "w");
        if (td->csv_file) {
            /* Version and timestamp header */
            char time_str[64];
            time_t now = time(NULL);
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));
            fprintf(td->csv_file, "# Phoenix SDR WWV Tick Log v%s\n", PHOENIX_VERSION_FULL);
            fprintf(td->csv_file, "# Started: %s\n", time_str);
            fprintf(td->csv_file, "time,timestamp_ms,tick_num,wwv_sec,expected,energy_peak,duration_ms,interval_ms,avg_interval_ms,noise_floor,corr_peak,corr_ratio\n");
            fflush(td->csv_file);
        }
    }

    printf("[TICK] Detector created: FFT=%d (%.1fms), matched filter=%d samples (%.1fms)\n",
           TICK_FFT_SIZE, FRAME_DURATION_MS, TICK_TEMPLATE_SAMPLES, TICK_PULSE_MS);
    printf("[TICK] Target: %dHz ±%dHz, logging to %s\n",
           TICK_TARGET_FREQ_HZ, TICK_BANDWIDTH_HZ, csv_path ? csv_path : "(disabled)");

    return td;
}

void tick_detector_destroy(tick_detector_t *td) {
    if (!td) return;

    if (td->wwv_clock) wwv_clock_destroy(td->wwv_clock);
    if (td->csv_file) fclose(td->csv_file);
    if (td->fft_cfg) kiss_fft_free(td->fft_cfg);
    free(td->fft_in);
    free(td->fft_out);
    free(td->window_func);
    free(td->i_buffer);
    free(td->q_buffer);
    free(td->template_i);
    free(td->template_q);
    free(td->corr_buf_i);
    free(td->corr_buf_q);
    free(td);
}

void tick_detector_set_callback(tick_detector_t *td, tick_callback_fn callback, void *user_data) {
    if (!td) return;
    td->callback = callback;
    td->callback_user_data = user_data;
}

bool tick_detector_process_sample(tick_detector_t *td, float i_sample, float q_sample) {
    if (!td || !td->detection_enabled) return false;

    /* Always feed correlation buffer (sample-by-sample) */
    td->corr_buf_i[td->corr_buf_idx] = i_sample;
    td->corr_buf_q[td->corr_buf_idx] = q_sample;
    td->corr_buf_idx = (td->corr_buf_idx + 1) % TICK_CORR_BUFFER_SIZE;
    td->corr_sample_count++;

    /* Compute correlation every N samples (for efficiency) */
    if (td->corr_sample_count >= TICK_TEMPLATE_SAMPLES &&
        (td->corr_sample_count % CORR_DECIMATION) == 0) {
        float corr = compute_correlation(td);

        /* Update correlation noise floor (slow adaptation) */
        if (corr < td->corr_noise_floor || td->corr_noise_floor < 0.001f) {
            td->corr_noise_floor += CORR_NOISE_ADAPT * (corr - td->corr_noise_floor);
        } else if (td->state == STATE_IDLE) {
            td->corr_noise_floor += (CORR_NOISE_ADAPT * 0.1f) * (corr - td->corr_noise_floor);
        }

        /* Track peak during detection */
        if (td->state == STATE_IN_TICK && corr > td->corr_peak) {
            td->corr_peak = corr;
            td->corr_peak_offset = td->corr_sample_count;
        }

        /* Accumulate correlation during detection */
        if (td->state == STATE_IN_TICK) {
            td->corr_sum += corr;
            td->corr_sum_count++;
        }
    }

    /* Buffer sample for FFT */
    td->i_buffer[td->buffer_idx] = i_sample;
    td->q_buffer[td->buffer_idx] = q_sample;
    td->buffer_idx++;

    /* Not enough samples yet */
    if (td->buffer_idx < TICK_FFT_SIZE) {
        return false;
    }

    /* Buffer full - run FFT */
    td->buffer_idx = 0;

    /* Apply window and load FFT input */
    for (int i = 0; i < TICK_FFT_SIZE; i++) {
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

    return (td->flash_frames_remaining == TICK_FLASH_FRAMES);
}

int tick_detector_get_flash_frames(tick_detector_t *td) {
    return td ? td->flash_frames_remaining : 0;
}

void tick_detector_decrement_flash(tick_detector_t *td) {
    if (td && td->flash_frames_remaining > 0) {
        td->flash_frames_remaining--;
    }
}

void tick_detector_set_enabled(tick_detector_t *td, bool enabled) {
    if (td) td->detection_enabled = enabled;
}

bool tick_detector_get_enabled(tick_detector_t *td) {
    return td ? td->detection_enabled : false;
}

float tick_detector_get_noise_floor(tick_detector_t *td) {
    return td ? td->noise_floor : 0.0f;
}

float tick_detector_get_threshold(tick_detector_t *td) {
    return td ? td->threshold_high : 0.0f;
}

float tick_detector_get_current_energy(tick_detector_t *td) {
    return td ? td->current_energy : 0.0f;
}

int tick_detector_get_tick_count(tick_detector_t *td) {
    return td ? td->ticks_detected : 0;
}

void tick_detector_print_stats(tick_detector_t *td) {
    if (!td) return;

    float elapsed = td->frame_count * FRAME_DURATION_MS / 1000.0f;
    float current_time_ms = td->frame_count * FRAME_DURATION_MS;
    float detecting = td->warmup_complete ?
        (elapsed - TICK_WARMUP_FRAMES * FRAME_DURATION_MS / 1000.0f) : 0.0f;
    int expected = (int)detecting;
    float rate = (expected > 0) ? (100.0f * td->ticks_detected / expected) : 0.0f;
    float avg_interval = calculate_avg_interval(td, current_time_ms);

    printf("\n=== TICK DETECTOR STATS ===\n");
    printf("FFT: %d (%.1fms), Matched filter: %d samples\n", TICK_FFT_SIZE, FRAME_DURATION_MS, TICK_TEMPLATE_SAMPLES);
    printf("Target: %d Hz +/-%d Hz\n", TICK_TARGET_FREQ_HZ, TICK_BANDWIDTH_HZ);
    printf("Elapsed: %.1fs  Detected: %d  Expected: %d  Rate: %.1f%%\n",
           elapsed, td->ticks_detected, expected, rate);
    printf("Markers: %d  Rejected: %d  Avg interval: %.0fms\n",
           td->markers_detected, td->ticks_rejected, avg_interval);
    printf("Energy noise: %.4f  Corr noise: %.2f\n", td->noise_floor, td->corr_noise_floor);
    printf("===========================\n");
}

void tick_detector_log_metadata(tick_detector_t *td, uint64_t center_freq,
                                uint32_t sample_rate, uint32_t gain_reduction,
                                uint32_t lna_state) {
    if (!td || !td->csv_file) return;

    /* Get current wall clock time */
    char time_str[64];
    time_t now = time(NULL);
    strftime(time_str, sizeof(time_str), "%H:%M:%S", localtime(&now));

    /* Get timestamp in ms since detector start */
    float timestamp_ms = td->frame_count * FRAME_DURATION_MS;

    /* Log as special META row */
    fprintf(td->csv_file, "%s,%.1f,META,0,freq=%llu rate=%u GR=%u LNA=%u,0,0,0,0,0,0\n",
            time_str, timestamp_ms,
            (unsigned long long)center_freq, sample_rate, gain_reduction, lna_state);
    fflush(td->csv_file);

    printf("[TICK] Logged metadata: freq=%llu, rate=%u, GR=%u, LNA=%u\n",
           (unsigned long long)center_freq, sample_rate, gain_reduction, lna_state);
}

void tick_detector_log_display_gain(tick_detector_t *td, float display_gain_db) {
    if (!td || !td->csv_file) return;

    /* Get current wall clock time */
    char time_str[64];
    time_t now = time(NULL);
    strftime(time_str, sizeof(time_str), "%H:%M:%S", localtime(&now));

    /* Get timestamp in ms since detector start */
    float timestamp_ms = td->frame_count * FRAME_DURATION_MS;

    /* Log as special GAIN row */
    fprintf(td->csv_file, "%s,%.1f,GAIN,0,display_gain=%.1f,0,0,0,0,0,0,0\n",
            time_str, timestamp_ms, display_gain_db);
    fflush(td->csv_file);
}

float tick_detector_get_frame_duration_ms(void) {
    return FRAME_DURATION_MS;
}
