/**
 * @file wwv_scan.c
 * @brief WWV/WWVH Frequency Scanner with GPS-Synchronized Tick Detection
 *
 * Scans all WWV/WWVH frequencies using GPS PPS timing to measure
 * signal energy in precise windows aligned to second boundaries.
 *
 * WWV Tick Structure (per second):
 *   |<--5ms TICK-->|<-----------995ms gap----------->|
 *   ^              ^                                  ^
 *   0ms           5ms                               1000ms
 *   (1000Hz tone)  (silence or voice/data)
 *
 * Detection Strategy:
 *   - Tick Window:  0-15ms after second boundary (capture 5ms tick + margin)
 *   - Noise Window: 100-900ms after second boundary (between ticks)
 *   - SNR = tick_energy / noise_energy
 *
 * Usage: wwv_scan [-scantime <sec>] [-p <COM>] [-g <gain>]
 */

#include "phoenix_sdr.h"
#include "decimator.h"
#include "gps_serial.h"
#include "version.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#ifdef _WIN32
#include <Windows.h>
#include <conio.h>  /* For _kbhit() and _getch() */
#define sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define sleep_ms(ms) usleep((ms) * 1000)
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*============================================================================
 * WWV/WWVH Frequencies
 *============================================================================*/

static const double WWV_FREQS_MHZ[] = {
    2.5,
    5.0,
    10.0,
    15.0,
    20.0,
    25.0
};
#define NUM_WWV_FREQS (sizeof(WWV_FREQS_MHZ) / sizeof(WWV_FREQS_MHZ[0]))

/*============================================================================
 * Configuration
 *============================================================================*/

#define SAMPLE_RATE_HZ      2000000.0
#define DECIMATED_RATE_HZ   48000.0
#define BANDWIDTH_KHZ       200
#define DEFAULT_SCAN_TIME   10      /* Seconds per frequency */
#define DEFAULT_GPS_PORT    "COM6"
#define DEFAULT_GAIN        40
#define AUTO_GAIN_DEFAULT   true    /* Auto-gain on by default */

/* GPS-aligned sample windows (in milliseconds) */
#define TICK_WINDOW_START_MS    0       /* Start of tick window */
#define TICK_WINDOW_END_MS      10      /* End of tick window (5ms tick + 5ms margin) */
#define NOISE_WINDOW_START_MS   200     /* Start of noise measurement (well after tick) */
#define NOISE_WINDOW_END_MS     800     /* End of noise measurement */

/* Convert ms to samples at decimated rate */
#define MS_TO_SAMPLES(ms) ((int)((ms) * DECIMATED_RATE_HZ / 1000.0))

/*============================================================================
 * Biquad Bandpass Filter for 1000 Hz detection
 *============================================================================*/

typedef struct {
    float b0, b1, b2;
    float a1, a2;
    float x1, x2;
    float y1, y2;
} biquad_t;

static void biquad_init_bp(biquad_t *bq, float fs, float fc, float Q) {
    float w0 = 2.0f * (float)M_PI * fc / fs;
    float alpha = sinf(w0) / (2.0f * Q);
    float cos_w0 = cosf(w0);

    float a0 = 1.0f + alpha;
    bq->b0 = alpha / a0;
    bq->b1 = 0.0f;
    bq->b2 = -alpha / a0;
    bq->a1 = -2.0f * cos_w0 / a0;
    bq->a2 = (1.0f - alpha) / a0;

    bq->x1 = bq->x2 = 0;
    bq->y1 = bq->y2 = 0;
}

static void biquad_reset(biquad_t *bq) {
    bq->x1 = bq->x2 = 0;
    bq->y1 = bq->y2 = 0;
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

/*============================================================================
 * DC Blocking Filter (single-pole highpass)
 * Removes DC from envelope to isolate modulation
 *============================================================================*/

typedef struct {
    float prev_in;
    float prev_out;
    float alpha;  /* 0.99 = ~0.16 Hz cutoff at 48kHz */
} dc_block_t;

static void dc_block_init(dc_block_t *dc, float alpha) {
    dc->prev_in = 0;
    dc->prev_out = 0;
    dc->alpha = alpha;
}

static void dc_block_reset(dc_block_t *dc) {
    dc->prev_in = 0;
    dc->prev_out = 0;
}

static float dc_block_process(dc_block_t *dc, float x) {
    /* y[n] = x[n] - x[n-1] + alpha * y[n-1] */
    float y = x - dc->prev_in + dc->alpha * dc->prev_out;
    dc->prev_in = x;
    dc->prev_out = y;
    return y;
}

/*============================================================================
 * Globals for streaming callback
 *============================================================================*/

static volatile bool g_running = true;
static volatile bool g_scanning = false;

/* Decimator */
static decim_state_t *g_decimator = NULL;
static decim_complex_t g_decim_buffer[8192];

/* DC blocking filter (removes carrier from envelope) */
static dc_block_t g_dc_block;

/* 1000 Hz bandpass filter */
static biquad_t g_bp_1000hz;

/* GPS timing state */
static gps_context_t g_gps_ctx;

/* Auto-gain state */
static psdr_context_t *g_sdr_ctx = NULL;
static psdr_config_t g_sdr_config;
static int g_overload_count = 0;
static bool g_auto_gain = AUTO_GAIN_DEFAULT;

/* Sample position within current second */
static volatile int g_samples_in_second = 0;

/* GPS resync handoff - main thread sets these, callback applies them */
static volatile bool g_resync_pending = false;
static volatile int g_resync_value = 0;

/* Window boundaries in samples */
static int g_tick_start_sample;
static int g_tick_end_sample;
static int g_noise_start_sample;
static int g_noise_end_sample;
static int g_samples_per_second;

/* Energy accumulators */
static double g_tick_energy_sum = 0.0;
static double g_tick_energy_peak = 0.0;  /* Peak energy during tick window */
static int g_tick_energy_count = 0;
static double g_noise_energy_sum = 0.0;
static double g_noise_energy_peak = 0.0;  /* Peak energy during noise window */
static int g_noise_energy_count = 0;

/* Per-second results */
#define MAX_SECONDS 120
static double g_tick_energy[MAX_SECONDS];
static double g_tick_peak[MAX_SECONDS];     /* Peak during tick window */
static double g_noise_energy[MAX_SECONDS];
static double g_noise_peak[MAX_SECONDS];    /* Peak during noise window */
static int g_seconds_measured = 0;

/* Dump mode: write one second of filtered values to file */
static bool g_dump_mode = false;
static FILE *g_dump_file = NULL;
static int g_dump_second = -1;  /* Which second to dump (-1 = first complete) */

/* DEBUG: Track signal levels through pipeline */
static float g_debug_max_raw = 0.0f;
static float g_debug_max_decim = 0.0f;
static float g_debug_max_mag = 0.0f;
static float g_debug_max_ac = 0.0f;
static float g_debug_max_filt = 0.0f;
static int g_debug_sample_count = 0;

/*============================================================================
 * Interactive Mode Globals
 *============================================================================*/

static bool g_interactive_mode = false;

/* Time buckets: 20 buckets of 50ms each = 1 second (for visualization) */
#define NUM_BUCKETS 20
#define BUCKET_MS   50
static double g_bucket_energy[NUM_BUCKETS];      /* Current second accumulator */
static int g_bucket_count[NUM_BUCKETS];          /* Sample counts per bucket */
static double g_bucket_display[NUM_BUCKETS];     /* Last complete second for display */
static volatile bool g_bucket_ready = false;     /* New data ready to display */

/*============================================================================
 * Moving Average Filter for Envelope Smoothing
 *============================================================================*/

#define MA_LENGTH 48  /* 1ms at 48kHz - smooths noise, preserves tick edge */
static float g_ma_buffer[MA_LENGTH];
static int g_ma_index = 0;
static float g_ma_sum = 0.0f;

static void ma_reset(void) {
    for (int i = 0; i < MA_LENGTH; i++) {
        g_ma_buffer[i] = 0.0f;
    }
    g_ma_index = 0;
    g_ma_sum = 0.0f;
}

static float ma_process(float x) {
    /* Subtract oldest, add newest */
    g_ma_sum -= g_ma_buffer[g_ma_index];
    g_ma_buffer[g_ma_index] = x;
    g_ma_sum += x;
    g_ma_index = (g_ma_index + 1) % MA_LENGTH;
    return g_ma_sum / MA_LENGTH;
}

/*============================================================================
 * Edge Detection State (FLDIGI-style: just track max derivative position)
 *============================================================================*/

static float g_prev_envelope = 0.0f;             /* Previous smoothed envelope for derivative */
static float g_noise_floor = 0.0f;               /* Running estimate of noise floor */
static volatile float g_max_derivative = 0.0f;   /* Max derivative seen this second */
static volatile float g_envelope_at_max_deriv = 0.0f; /* Envelope when max derivative occurred */
static volatile int g_max_deriv_sample_pos = -1;     /* Sample position of max derivative */

/* Display copies - saved at second boundary before reset */
static float g_max_derivative_display = 0.0f;
static float g_envelope_at_max_deriv_display = 0.0f;
static int g_max_deriv_sample_pos_display = -1;
static float g_noise_floor_display = 0.0f;  /* Snapshot of noise floor for display */

/* PPS offset: manual adjustment to align with tick */
static volatile int g_pps_offset_ms = -440;      /* Calibrated default for this system */

/* Discipline mode: GPS (default) or WWV */
static bool g_wwv_disciplined = false;           /* false=GPS steers timing, true=WWV steers timing */
#define WWV_SNR_THRESHOLD_DB  0.0                /* Minimum SNR to trust tick detection (0 = any positive) */
#define WWV_CORRECTION_FACTOR 4                  /* Divide error by this (25% correction per sec) */

/* Current frequency index for interactive mode */
static int g_freq_index = 3;                     /* Default to 15 MHz (index 3) */

/* Hit rate tracking for interactive mode */
static int g_hit_count = 0;      /* Peaks in bucket 0 or 1 */
static int g_total_count = 0;    /* Total seconds measured */
static int g_clean_hit_count = 0;   /* Hits during clean windows only */
static int g_clean_total_count = 0; /* Clean window seconds only */

/* Last GPS reading for display */
static gps_reading_t g_last_gps;
static int g_last_gps_ms = 0;                    /* GPS millisecond when second started */

/*============================================================================
 * Signal Handler
 *============================================================================*/

static void signal_handler(int sig) {
    (void)sig;
    printf("\nInterrupt received, stopping...\n");
    g_running = false;
    g_scanning = false;
}

/*============================================================================
 * Streaming Callback - GPS-Synchronized Window Detection
 *============================================================================*/

static void on_samples(
    const int16_t *xi,
    const int16_t *xq,
    uint32_t count,
    bool reset,
    void *user_ctx
) {
    (void)user_ctx;

    if (reset && g_decimator) {
        decim_reset(g_decimator);
        biquad_reset(&g_bp_1000hz);
        ma_reset();
        g_prev_envelope = 0.0f;
    }

    if (!g_scanning) return;

    /* Check for GPS resync from main thread - adjusts timing only */
    if (g_resync_pending) {
        g_samples_in_second = g_resync_value;
        g_resync_pending = false;
        /* Note: per-second reset happens when sample count reaches g_samples_per_second */
    }

    /* DEBUG: Track raw sample range */
    for (uint32_t j = 0; j < count && j < 100; j++) {
        float raw_mag = sqrtf((float)xi[j]*xi[j] + (float)xq[j]*xq[j]);
        if (raw_mag > g_debug_max_raw) g_debug_max_raw = raw_mag;
    }

    /* Decimate to 48kHz */
    size_t out_count = 0;
    decim_error_t derr = decim_process_int16(
        g_decimator, xi, xq, count,
        g_decim_buffer, sizeof(g_decim_buffer)/sizeof(g_decim_buffer[0]),
        &out_count
    );

    if (derr != DECIM_OK || out_count == 0) return;

    /* Process each decimated sample */
    for (size_t i = 0; i < out_count; i++) {
        /* DEBUG: Track decimated I/Q magnitude */
        float decim_mag = sqrtf(g_decim_buffer[i].i * g_decim_buffer[i].i +
                               g_decim_buffer[i].q * g_decim_buffer[i].q);
        if (decim_mag > g_debug_max_decim) g_debug_max_decim = decim_mag;

        /* AM demodulation: envelope = sqrt(I² + Q²) */
        float mag = decim_mag;  /* Already computed above */
        if (mag > g_debug_max_mag) g_debug_max_mag = mag;

        /* Skip DC blocking - bandpass filter rejects DC anyway */
        /* Feed envelope directly to 1000 Hz bandpass */
        float filtered = biquad_process(&g_bp_1000hz, mag);
        g_debug_max_ac = g_debug_max_mag;  /* For debug output consistency */
        if (fabsf(filtered) > g_debug_max_filt) g_debug_max_filt = fabsf(filtered);

        float energy = filtered * filtered;  /* Power */
        g_debug_sample_count++;

        /* Moving average smoothing for edge detection */
        float envelope_smoothed = ma_process(fabsf(filtered));

        /* Determine which window this sample falls into */
        int sample_pos = g_samples_in_second;

        if (sample_pos >= g_tick_start_sample && sample_pos < g_tick_end_sample) {
            /* In tick window */
            g_tick_energy_sum += energy;
            g_tick_energy_count++;
            if (energy > g_tick_energy_peak) g_tick_energy_peak = energy;
        }
        else if (sample_pos >= g_noise_start_sample && sample_pos < g_noise_end_sample) {
            /* In noise window */
            g_noise_energy_sum += energy;
            g_noise_energy_count++;
            if (energy > g_noise_energy_peak) g_noise_energy_peak = energy;
        }

        /* Dump mode: write sample data for one second */
        if (g_dump_mode && g_dump_file && g_seconds_measured == g_dump_second) {
            fprintf(g_dump_file, "%d,%.6f,%.6f,%.9f,%.9f\n",
                    sample_pos, g_decim_buffer[i].i, g_decim_buffer[i].q, mag, filtered);
        }

        /* Interactive mode: edge detection + bucket visualization */
        if (g_interactive_mode) {
            /* Apply PPS offset to sample position */
            int adjusted_pos = sample_pos + MS_TO_SAMPLES(g_pps_offset_ms);
            if (adjusted_pos < 0) adjusted_pos += g_samples_per_second;
            if (adjusted_pos >= g_samples_per_second) adjusted_pos -= g_samples_per_second;

            /* Edge detection: look for sharp rise in smoothed envelope */
            float derivative = envelope_smoothed - g_prev_envelope;
            g_prev_envelope = envelope_smoothed;

            /* Track max derivative for debug */
            if (derivative > g_max_derivative) {
                g_max_derivative = derivative;
                g_envelope_at_max_deriv = envelope_smoothed;
                g_max_deriv_sample_pos = adjusted_pos;
            }

            /* NOTE: Noise floor is now calculated from buckets at second boundary */
            /* Old IIR method removed - was tracking wrong window due to PPS offset */

            /* FLDIGI-style: No gating - max derivative position IS the tick position */
            /* We already track g_max_derivative and g_max_deriv_sample_pos above */
            /* Human identifies pattern visually from E position each second */

            /* Still fill buckets for visualization */
            int bucket = ((adjusted_pos % g_samples_per_second) * NUM_BUCKETS) / g_samples_per_second;
            if (bucket >= 0 && bucket < NUM_BUCKETS) {
                g_bucket_energy[bucket] += (double)(envelope_smoothed * envelope_smoothed);
                g_bucket_count[bucket]++;
            }
        }

        g_samples_in_second++;

        /* Detect second boundary by sample count - works in BOTH GPS and WWV modes */
        if (g_samples_in_second >= g_samples_per_second) {
            /* Save values for display BEFORE reset */
            g_max_derivative_display = g_max_derivative;
            g_envelope_at_max_deriv_display = g_envelope_at_max_deriv;
            g_max_deriv_sample_pos_display = g_max_deriv_sample_pos;

            /* Also save bucket data for display */
            for (int b = 0; b < NUM_BUCKETS; b++) {
                if (g_bucket_count[b] > 0) {
                    g_bucket_display[b] = g_bucket_energy[b] / g_bucket_count[b];
                } else {
                    g_bucket_display[b] = 0.0;
                }
                g_bucket_energy[b] = 0.0;
                g_bucket_count[b] = 0;
            }

            /* Reset for new second */
            g_samples_in_second = 0;
            g_max_derivative = 0.0f;
            g_envelope_at_max_deriv = 0.0f;
            g_max_deriv_sample_pos = -1;

            /* Signal that new data is ready */
            g_bucket_ready = true;
        }

        /* NO WRAP HERE - GPS resync is the sole time authority */
    }
}

static void on_gain_change(double gain_db, int lna_db, void *user_ctx) {
    (void)user_ctx; (void)gain_db; (void)lna_db;
}

static void on_overload(bool overloaded, void *user_ctx) {
    (void)user_ctx;

    if (overloaded) {
        g_overload_count++;

        if (g_auto_gain && g_sdr_ctx) {
            bool adjusted = false;

            /* First try increasing gain reduction */
            if (g_sdr_config.gain_reduction < 59) {
                int new_gr = g_sdr_config.gain_reduction + 3;
                if (new_gr > 59) new_gr = 59;
                g_sdr_config.gain_reduction = new_gr;
                adjusted = true;
            }
            /* Then try increasing LNA state */
            else if (g_sdr_config.lna_state < 8) {
                g_sdr_config.lna_state++;
                adjusted = true;
            }

            if (adjusted) {
                psdr_update(g_sdr_ctx, &g_sdr_config);
                printf("[AG:%ddB,L%d]", g_sdr_config.gain_reduction, g_sdr_config.lna_state);
                fflush(stdout);
            }
            /* Suppress "at max" messages - too noisy for scanning */
        } else {
            printf("!");
            fflush(stdout);
        }
    }
}

/*============================================================================
 * Scan Result Structure
 *============================================================================*/

typedef struct {
    double freq_mhz;
    double tick_energy_avg;
    double noise_energy_avg;
    double snr_db;
    int seconds_measured;
    int ticks_detected;         /* Ticks where energy > noise threshold */
    int overloads;              /* Overload events during scan */
    int final_gain;             /* Final gain reduction after auto-gain */
    int final_lna;              /* Final LNA state after auto-gain */
    bool scanned;
} scan_result_t;

/*============================================================================
 * Scan a Single Frequency
 *============================================================================*/

static int scan_frequency(psdr_context_t *ctx, double freq_mhz, int scan_seconds,
                          int gain, scan_result_t *result) {
    psdr_error_t err;

    memset(result, 0, sizeof(*result));
    result->freq_mhz = freq_mhz;
    result->scanned = false;

    /* Configure for this frequency */
    psdr_config_defaults(&g_sdr_config);
    g_sdr_config.freq_hz = freq_mhz * 1e6;
    g_sdr_config.sample_rate_hz = SAMPLE_RATE_HZ;
    g_sdr_config.bandwidth = BANDWIDTH_KHZ;
    g_sdr_config.agc_mode = PSDR_AGC_DISABLED;
    g_sdr_config.gain_reduction = gain;
    g_sdr_config.lna_state = 0;
    g_sdr_config.antenna = PSDR_ANT_HIZ;

    /* Store context for auto-gain callback */
    g_sdr_ctx = ctx;
    g_overload_count = 0;

    printf("\n  %.1f MHz: tuning... ", freq_mhz);
    fflush(stdout);

    err = psdr_configure(ctx, &g_sdr_config);
    if (err != PSDR_OK) {
        printf("FAILED\n");
        return -1;
    }

    /* Reset detection state */
    /* DC blocking removed - bandpass filter rejects DC naturally */
    biquad_init_bp(&g_bp_1000hz, (float)DECIMATED_RATE_HZ, 1000.0f, 2.0f);  /* Q=2, BW=500Hz, faster edge response */
    g_tick_energy_sum = 0.0;
    g_tick_energy_peak = 0.0;
    g_tick_energy_count = 0;
    g_noise_energy_sum = 0.0;
    g_noise_energy_peak = 0.0;
    g_noise_energy_count = 0;
    g_resync_value = 0;
    g_resync_pending = true;
    g_seconds_measured = 0;

    /* Reset debug tracking */
    g_debug_max_raw = 0.0f;
    g_debug_max_decim = 0.0f;
    g_debug_max_mag = 0.0f;
    g_debug_max_ac = 0.0f;
    g_debug_max_filt = 0.0f;
    g_debug_sample_count = 0;

    if (g_decimator) {
        decim_reset(g_decimator);
    }

    /* Calculate window boundaries in samples */
    g_samples_per_second = (int)DECIMATED_RATE_HZ;
    g_tick_start_sample = MS_TO_SAMPLES(TICK_WINDOW_START_MS);
    g_tick_end_sample = MS_TO_SAMPLES(TICK_WINDOW_END_MS);
    g_noise_start_sample = MS_TO_SAMPLES(NOISE_WINDOW_START_MS);
    g_noise_end_sample = MS_TO_SAMPLES(NOISE_WINDOW_END_MS);

    /* Sync to GPS second boundary */
    printf("GPS sync... ");
    fflush(stdout);

    gps_reading_t gps;
    bool synced = false;

    for (int attempt = 0; attempt < 30 && g_running; attempt++) {
        if (gps_read_time(&g_gps_ctx, &gps, 100) == 0 && gps.valid) {
            if (gps.millisecond < 50) {
                /* Near second boundary - good sync point */
                g_resync_value = MS_TO_SAMPLES(gps.millisecond);
                g_resync_pending = true;
                synced = true;
                break;
            }
        }
        sleep_ms(20);
    }

    if (!synced) {
        printf("SYNC FAILED\n");
        return -1;
    }

    /* Set up callbacks and start streaming */
    psdr_callbacks_t callbacks = {
        .on_samples = on_samples,
        .on_gain_change = on_gain_change,
        .on_overload = on_overload,
        .user_ctx = NULL
    };

    printf("scanning ");
    fflush(stdout);

    g_scanning = true;
    err = psdr_start(ctx, &callbacks);
    if (err != PSDR_OK) {
        printf("START FAILED\n");
        g_scanning = false;
        return -1;
    }

    /* Collect data for scan_seconds, re-syncing each second */
    int seconds_collected = 0;
    int last_gps_second = -1;

    while (seconds_collected < scan_seconds && g_running) {
        if (gps_read_time(&g_gps_ctx, &gps, 200) == 0 && gps.valid) {
            if (gps.second != last_gps_second && last_gps_second >= 0) {
                /* Second boundary crossed - save results and reset */
                if (g_tick_energy_count > 0 && g_noise_energy_count > 0 &&
                    g_seconds_measured < MAX_SECONDS) {

                    double tick_avg = g_tick_energy_sum / g_tick_energy_count;
                    double noise_avg = g_noise_energy_sum / g_noise_energy_count;
                    g_tick_energy[g_seconds_measured] = tick_avg;
                    g_tick_peak[g_seconds_measured] = g_tick_energy_peak;
                    g_noise_energy[g_seconds_measured] = noise_avg;
                    g_noise_peak[g_seconds_measured] = g_noise_energy_peak;

                    /* DEBUG: Show per-second results with PEAK SNR */
                    double sec_snr_avg = (noise_avg > 1e-12) ? 10.0 * log10(tick_avg / noise_avg) : 0.0;
                    double sec_snr_peak = (g_noise_energy_peak > 1e-12) ? 10.0 * log10(g_tick_energy_peak / g_noise_energy_peak) : 0.0;
                    printf("\n    [sec %d: avgSNR=%+.1fdB peakSNR=%+.1fdB tick_pk=%.2e noise_pk=%.2e ms=%d]",
                           g_seconds_measured, sec_snr_avg, sec_snr_peak,
                           g_tick_energy_peak, g_noise_energy_peak, gps.millisecond);

                    g_seconds_measured++;
                }

                /* Reset for next second */
                g_tick_energy_sum = 0.0;
                g_tick_energy_peak = 0.0;
                g_tick_energy_count = 0;
                g_noise_energy_sum = 0.0;
                g_noise_energy_peak = 0.0;
                g_noise_energy_count = 0;

                /* Re-sync sample counter to GPS */
                g_resync_value = MS_TO_SAMPLES(gps.millisecond);
                g_resync_pending = true;

                seconds_collected++;
                printf(".");
                fflush(stdout);
            }
            last_gps_second = gps.second;
        }
        sleep_ms(10);
    }

    /* Stop streaming */
    g_scanning = false;
    psdr_stop(ctx);

    /* Calculate results */
    if (g_seconds_measured > 0) {
        double tick_sum = 0.0, noise_sum = 0.0;
        int ticks_detected = 0;

        for (int i = 0; i < g_seconds_measured; i++) {
            tick_sum += g_tick_energy[i];
            noise_sum += g_noise_energy[i];

            /* Count ticks where energy significantly exceeds noise */
            if (g_tick_energy[i] > g_noise_energy[i] * 2.0) {
                ticks_detected++;
            }
        }

        result->tick_energy_avg = tick_sum / g_seconds_measured;
        result->noise_energy_avg = noise_sum / g_seconds_measured;
        result->seconds_measured = g_seconds_measured;
        result->ticks_detected = ticks_detected;

        /* Calculate SNR in dB */
        if (result->noise_energy_avg > 1e-12) {
            result->snr_db = 10.0 * log10(result->tick_energy_avg / result->noise_energy_avg);
        } else {
            result->snr_db = 0.0;
        }

        /* Save auto-gain results */
        result->overloads = g_overload_count;
        result->final_gain = g_sdr_config.gain_reduction;
        result->final_lna = g_sdr_config.lna_state;

        result->scanned = true;
    }

    printf(" done\n");

    /* DEBUG: Print signal levels at each stage */
    printf("  DEBUG: Samples=%d  Raw=%.0f  Decim=%.4f  Mag=%.4f  AC=%.6f  Filt=%.6f\n",
           g_debug_sample_count, g_debug_max_raw, g_debug_max_decim,
           g_debug_max_mag, g_debug_max_ac, g_debug_max_filt);

    printf("         Tick: %.2e  Noise: %.2e  SNR: %+.1f dB  Ticks: %d/%d",
           result->tick_energy_avg, result->noise_energy_avg,
           result->snr_db, result->ticks_detected, result->seconds_measured);
    if (g_overload_count > 0) {
        printf("  [OL:%d->%ddB,L%d]", g_overload_count,
               g_sdr_config.gain_reduction, g_sdr_config.lna_state);
    }
    printf("\n");

    return 0;
}

/*============================================================================
 * Interactive Mode
 *============================================================================*/

static void interactive_reset_buckets(void) {
    for (int i = 0; i < NUM_BUCKETS; i++) {
        g_bucket_energy[i] = 0.0;
        g_bucket_count[i] = 0;
    }
    /* Max derivative tracking is reset in callback when g_resync_pending is processed */
    /* Note: don't reset g_prev_envelope or g_noise_floor - they carry over */
}

static void interactive_display(void) {
    /* FLDIGI-style: Use max derivative position directly (no gating) */
    int tick_pos = g_max_deriv_sample_pos_display;
    float tick_deriv = g_max_derivative_display;

    /* Convert tick sample position to ms for display */
    int tick_ms = (tick_pos >= 0) ? (tick_pos * 1000 / g_samples_per_second) : -1;

    /* Convert to bucket for visualization */
    int tick_bucket = (tick_pos >= 0) ? (tick_pos * NUM_BUCKETS / g_samples_per_second) : -1;
    if (tick_bucket >= NUM_BUCKETS) tick_bucket = NUM_BUCKETS - 1;

    /* Also find max energy bucket (for comparison/fallback) */
    double max_energy = 0.0;
    int max_bucket = 0;
    for (int i = 0; i < NUM_BUCKETS; i++) {
        if (g_bucket_display[i] > max_energy) {
            max_energy = g_bucket_display[i];
            max_bucket = i;
        }
    }
    if (max_energy < 1e-12) max_energy = 1e-12;

    /* Voice window gate: seconds 0, 29-51, 59 are garbage */
    int gps_sec = g_last_gps.second;
    bool in_clean_window = (gps_sec >= 1 && gps_sec <= 28) || (gps_sec >= 52 && gps_sec <= 58);

    /* Calculate SNR: bucket[0] vs average of buckets 4-19 */
    double noise_avg = 0.0;
    for (int i = 4; i < NUM_BUCKETS; i++) {
        noise_avg += g_bucket_display[i];
    }
    noise_avg /= (NUM_BUCKETS - 4);
    double snr = (noise_avg > 1e-12) ? 10.0 * log10(g_bucket_display[0] / noise_avg) : 0.0;

    /* WWV-disciplined mode: apply timing correction based on max derivative position */
    if (g_wwv_disciplined && in_clean_window && tick_pos >= 0) {
        /* We expect tick edge at sample 0. Calculate error in samples. */
        int error_samples = tick_pos;
        if (error_samples > g_samples_per_second / 2) {
            /* Wrapped - we're fast, not slow */
            error_samples = error_samples - g_samples_per_second;
        }

        /* Convert to ms for display, apply correction */
        int error_ms = (error_samples * 1000) / g_samples_per_second;

        /* NEGATIVE correction: if tick at sample N (we're late), subtract from counter */
        int correction_samples = -error_samples / WWV_CORRECTION_FACTOR;
        int correction_ms = (correction_samples * 1000) / g_samples_per_second;

        printf("[WWV] pos=%dms err=%+dms corr=%+dms ", tick_ms, error_ms, correction_ms);

        if (correction_samples != 0) {
            /* Apply correction via resync mechanism (thread-safe) */
            int current = g_samples_in_second;  /* Read once */
            int new_value = current + correction_samples;
            /* Handle wrap */
            if (new_value < 0) new_value += g_samples_per_second;
            if (new_value >= g_samples_per_second) new_value -= g_samples_per_second;
            g_resync_value = new_value;
            g_resync_pending = true;
            printf("applied\n");
        } else {
            printf("skip\n");
        }
    }

    /* Track hit rate - max derivative in tick window (0-50ms) counts as hit */
    g_total_count++;
    bool is_hit = (tick_ms >= 0 && tick_ms < 50);  /* Max derivative in first 50ms */
    if (is_hit) g_hit_count++;

    /* Track clean-window stats separately */
    if (in_clean_window) {
        g_clean_total_count++;
        if (is_hit) g_clean_hit_count++;
    }

    int hit_pct = (g_total_count > 0) ? (100 * g_hit_count / g_total_count) : 0;
    int clean_pct = (g_clean_total_count > 0) ? (100 * g_clean_hit_count / g_clean_total_count) : 0;

    /* One-line output: show max derivative position with ASCII bar */
    /* E = max derivative position, # = max energy bucket (for comparison) */
    printf("%.1fMHz %c G%d O%+4d |",
           WWV_FREQS_MHZ[g_freq_index],
           g_wwv_disciplined ? 'W' : 'G',  /* Mode indicator */
           g_sdr_config.gain_reduction,
           g_pps_offset_ms);

    for (int i = 0; i < NUM_BUCKETS; i++) {
        if (tick_bucket >= 0 && i == tick_bucket) {
            printf("E");  /* Edge-detected position */
        } else if (i == max_bucket) {
            printf("#");  /* Max energy bucket (for comparison) */
        } else if (i == 0) {
            printf("|");  /* Mark bucket 0 target */
        } else {
            printf("-");
        }
    }

    /* Show tick detection info - always show max derivative position */
    printf("| s%02d %c @%3dms d%.2e H%2d%% C%2d%%\n",
           gps_sec,
           in_clean_window ? ' ' : 'V',
           tick_ms,
           tick_deriv,
           hit_pct, clean_pct);

    /* DIAGNOSTIC: Show debug info */
    float nf15 = g_noise_floor_display * 1.5f;
    bool in_tick_window = (tick_ms >= 0 && tick_ms < 50);

    printf("  [DBG] env=%.2e nf=%.2e nf*1.5=%.2e | %s\n",
           g_envelope_at_max_deriv_display,
           g_noise_floor_display,
           nf15,
           in_tick_window ? "IN-WINDOW" : "out-of-window");
}

static int run_interactive_mode(psdr_context_t *ctx, int initial_gain, const char *gps_port) {
    psdr_error_t err;
    (void)gps_port;

    printf("Keys: 1-6=freq +/-=offset <>=gain G=mode R=reset Q=quit\n");

    /* Configure initial frequency */
    psdr_config_defaults(&g_sdr_config);
    g_sdr_config.freq_hz = WWV_FREQS_MHZ[g_freq_index] * 1e6;
    g_sdr_config.sample_rate_hz = SAMPLE_RATE_HZ;
    g_sdr_config.bandwidth = BANDWIDTH_KHZ;
    g_sdr_config.agc_mode = PSDR_AGC_DISABLED;
    g_sdr_config.gain_reduction = initial_gain;
    g_sdr_config.lna_state = 0;
    g_sdr_config.antenna = PSDR_ANT_HIZ;
    g_sdr_ctx = ctx;

    err = psdr_configure(ctx, &g_sdr_config);
    if (err != PSDR_OK) {
        printf("Failed to configure SDR\n");
        return -1;
    }

    /* Initialize DSP */
    biquad_init_bp(&g_bp_1000hz, (float)DECIMATED_RATE_HZ, 1000.0f, 2.0f);  /* Q=2, BW=500Hz, faster edge response */
    ma_reset();
    g_prev_envelope = 0.0f;
    g_noise_floor = 0.0f;
    interactive_reset_buckets();
    g_samples_per_second = (int)DECIMATED_RATE_HZ;
    g_interactive_mode = true;
    g_hit_count = 0;
    g_total_count = 0;
    g_clean_hit_count = 0;
    g_clean_total_count = 0;

    /* Initial GPS sync - read current position in second */
    gps_reading_t gps;
    if (gps_read_time(&g_gps_ctx, &gps, 500) == 0 && gps.valid) {
        g_resync_value = MS_TO_SAMPLES(gps.millisecond);
        g_resync_pending = true;
    } else {
        g_resync_value = 0;
        g_resync_pending = true;
        printf("Warning: No GPS sync, starting at 0\n");
    }

    if (g_decimator) decim_reset(g_decimator);

    /* Set up callbacks and start streaming */
    psdr_callbacks_t callbacks = {
        .on_samples = on_samples,
        .on_gain_change = on_gain_change,
        .on_overload = on_overload,
        .user_ctx = NULL
    };

    g_scanning = true;
    err = psdr_start(ctx, &callbacks);
    if (err != PSDR_OK) {
        printf("Failed to start streaming\n");
        return -1;
    }

    int last_gps_second = -1;
    bool need_reconfig = false;

    /* Main interactive loop */
    while (g_running) {
        /* Check for keyboard input */
#ifdef _WIN32
        if (_kbhit()) {
            int ch = _getch();
#else
        int ch = getchar();
        if (ch != EOF) {
#endif
            switch (ch) {
                case 'q': case 'Q':
                    g_running = false;
                    break;

                case '1': g_freq_index = 0; need_reconfig = true; break;  /* 2.5 MHz */
                case '2': g_freq_index = 1; need_reconfig = true; break;  /* 5.0 MHz */
                case '3': g_freq_index = 2; need_reconfig = true; break;  /* 10 MHz */
                case '4': g_freq_index = 3; need_reconfig = true; break;  /* 15 MHz */
                case '5': g_freq_index = 4; need_reconfig = true; break;  /* 20 MHz */
                case '6': g_freq_index = 5; need_reconfig = true; break;  /* 25 MHz */

                case '+': case '=':
                    g_pps_offset_ms += 10;
                    if (g_pps_offset_ms > 500) g_pps_offset_ms = 500;
                    break;
                case '-': case '_':
                    g_pps_offset_ms -= 10;
                    if (g_pps_offset_ms < -500) g_pps_offset_ms = -500;
                    break;

                case '>': case '.':
                    if (g_sdr_config.gain_reduction > 20) {
                        g_sdr_config.gain_reduction -= 3;
                        psdr_update(ctx, &g_sdr_config);
                    }
                    break;
                case '<': case ',':
                    if (g_sdr_config.gain_reduction < 59) {
                        g_sdr_config.gain_reduction += 3;
                        psdr_update(ctx, &g_sdr_config);
                    }
                    break;

                case 'r': case 'R':
                    g_pps_offset_ms = -440;  /* Reset to calibrated default */
                    break;

                case 'g': case 'G':
                    g_wwv_disciplined = !g_wwv_disciplined;
                    printf("[MODE] %s\n", g_wwv_disciplined ? "WWV-disciplined" : "GPS-disciplined");
                    break;
            }
        }

        /* Reconfigure if frequency changed */
        if (need_reconfig) {
            g_scanning = false;
            psdr_stop(ctx);

            g_sdr_config.freq_hz = WWV_FREQS_MHZ[g_freq_index] * 1e6;
            psdr_configure(ctx, &g_sdr_config);

            biquad_reset(&g_bp_1000hz);
            ma_reset();
            g_prev_envelope = 0.0f;
            g_noise_floor = 0.0f;
            if (g_decimator) decim_reset(g_decimator);
            interactive_reset_buckets();

            /* Reset hit counters */
            g_hit_count = 0;
            g_total_count = 0;
            g_clean_hit_count = 0;
            g_clean_total_count = 0;

            g_scanning = true;
            psdr_start(ctx, &callbacks);
            need_reconfig = false;
        }

        /* Read GPS and detect second boundary */
        gps_reading_t gps;
        if (gps_read_time(&g_gps_ctx, &gps, 50) == 0 && gps.valid) {
            if (gps.second != last_gps_second && last_gps_second >= 0) {
                /* Second boundary - calculate noise floor from bucket display data */
                /* Note: bucket display values are saved by callback at sample boundary */

                /* Calculate noise floor from quiet buckets (4-16 = 200-850ms adjusted) */
                /* These buckets avoid tick (0-3) and pre-tick (17-19) */
                double noise_sum = 0.0;
                int noise_count = 0;
                for (int i = 4; i <= 16; i++) {
                    if (g_bucket_count[i] > 0) {
                        noise_sum += g_bucket_display[i];  /* Use averaged values */
                        noise_count++;
                    }
                }
                if (noise_count > 0) {
                    g_noise_floor = (float)sqrt(noise_sum / noise_count);  /* sqrt because buckets store energy (squared) */
                }
                g_noise_floor_display = g_noise_floor;

                /* Note: max derivative display values are saved by callback at sample boundary */
                /* Note: bucket reset also happens in callback */

                /* Re-sync sample counter to GPS - only in GPS-disciplined mode */
                if (!g_wwv_disciplined) {
                    int expected = g_samples_per_second;
                    int drift = g_samples_in_second - expected;
                    if (drift > 4800 || drift < -4800) {  /* >100ms drift - only print if significant */
                        printf("[DRIFT:%+dms]", (drift * 1000) / g_samples_per_second);
                    }
                    g_resync_value = MS_TO_SAMPLES(gps.millisecond);
                    g_resync_pending = true;
                }
                g_last_gps_ms = gps.millisecond;
                memcpy(&g_last_gps, &gps, sizeof(gps));

                /* Update display */
                interactive_display();
            }
            last_gps_second = gps.second;
        }

        sleep_ms(10);
    }

    g_scanning = false;
    g_interactive_mode = false;
    psdr_stop(ctx);

    printf("\n\nInteractive mode ended.\n");
    printf("Final PPS offset: %+d ms\n", g_pps_offset_ms);

    return 0;
}

/*============================================================================
 * Usage
 *============================================================================*/

static void print_usage(const char *progname) {
    printf("WWV/WWVH Frequency Scanner (GPS-Synchronized)\n");
    printf("Scans all WWV frequencies using GPS PPS timing\n\n");
    printf("Usage: %s [options]\n\n", progname);
    printf("Options:\n");
    printf("  -scantime <sec>   Seconds per frequency (default: %d)\n", DEFAULT_SCAN_TIME);
    printf("  -p, --gps <COM>   GPS port (default: %s)\n", DEFAULT_GPS_PORT);
    printf("  -g, --gain <dB>   Gain reduction 20-59 (default: %d)\n", DEFAULT_GAIN);
    printf("  --no-auto-gain    Disable automatic gain adjustment\n");
    printf("  --dump <file>     Dump one second of waveform to CSV\n");
    printf("  -i, --interactive Interactive mode - adjust settings live\n");
    printf("  -h, --help        Show this help\n\n");
    printf("Detection Method:\n");
    printf("  Each Second (1000ms):\n");
    printf("  +-- TICK WINDOW (%d-%dms) --------+\n", TICK_WINDOW_START_MS, TICK_WINDOW_END_MS);
    printf("  |  [####]................       | <- 5ms 1000Hz tick lives here\n");
    printf("  |  Measure tick energy here     |\n");
    printf("  +--------------------------------+\n");
    printf("  |\n");
    printf("  +-- (skip %d-%dms settling)\n", TICK_WINDOW_END_MS, NOISE_WINDOW_START_MS);
    printf("  |\n");
    printf("  +-- NOISE WINDOW (%d-%dms) ------+\n", NOISE_WINDOW_START_MS, NOISE_WINDOW_END_MS);
    printf("  |  ............................ | <- Background noise only\n");
    printf("  |  Measure noise floor here     |\n");
    printf("  +--------------------------------+\n");
    printf("  |\n");
    printf("  +-- (skip %d-1000ms, next tick)\n\n", NOISE_WINDOW_END_MS);
    printf("  SNR = 10 * log10(tick_energy / noise_energy)\n\n");
    printf("WWV Frequencies: 2.5, 5.0, 10.0, 15.0, 20.0, 25.0 MHz\n");
}

/*============================================================================
 * Main
 *============================================================================*/

int main(int argc, char *argv[]) {
    int scan_time = DEFAULT_SCAN_TIME;
    char gps_port[32] = DEFAULT_GPS_PORT;
    int gain = DEFAULT_GAIN;
    char dump_file[256] = "";
    bool interactive = false;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        else if (strcmp(argv[i], "-scantime") == 0 && i + 1 < argc) {
            scan_time = atoi(argv[++i]);
            if (scan_time < 3) scan_time = 3;
            if (scan_time > 120) scan_time = 120;
        }
        else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--gps") == 0) && i + 1 < argc) {
            strncpy(gps_port, argv[++i], sizeof(gps_port) - 1);
        }
        else if ((strcmp(argv[i], "-g") == 0 || strcmp(argv[i], "--gain") == 0) && i + 1 < argc) {
            gain = atoi(argv[++i]);
            if (gain < 20) gain = 20;
            if (gain > 59) gain = 59;
        }
        else if (strcmp(argv[i], "--no-auto-gain") == 0) {
            g_auto_gain = false;
        }
        else if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
            strncpy(dump_file, argv[++i], sizeof(dump_file) - 1);
            g_dump_mode = true;
            g_dump_second = 0;  /* Dump first complete second */
        }
        else if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--interactive") == 0) {
            interactive = true;
        }
    }

    /* Open dump file if requested */
    if (g_dump_mode && dump_file[0]) {
        g_dump_file = fopen(dump_file, "w");
        if (!g_dump_file) {
            fprintf(stderr, "Failed to open dump file: %s\n", dump_file);
            return 1;
        }
        fprintf(g_dump_file, "sample,I,Q,envelope,filtered_1000Hz\n");
        printf("Dump mode: Will write second %d to %s\n", g_dump_second, dump_file);
    }

    print_version("wwv_scan");
    printf("===========================================\n");
    printf("WWV/WWVH Frequency Scanner\n");
    printf("GPS-Synchronized Tick Detection\n");
    printf("===========================================\n\n");

    printf("Each Second (1000ms):\n");
    printf("+-- TICK WINDOW (%d-%dms) --------+\n", TICK_WINDOW_START_MS, TICK_WINDOW_END_MS);
    printf("|  [####]................       | <- 5ms 1000Hz tick\n");
    printf("+--------------------------------+\n");
    printf("|\n");
    printf("+-- (skip %d-%dms settling)\n", TICK_WINDOW_END_MS, NOISE_WINDOW_START_MS);
    printf("|\n");
    printf("+-- NOISE WINDOW (%d-%dms) ------+\n", NOISE_WINDOW_START_MS, NOISE_WINDOW_END_MS);
    printf("|  ............................ | <- Background noise\n");
    printf("+--------------------------------+\n");
    printf("|\n");
    printf("+-- (skip %d-1000ms, next tick)\n\n", NOISE_WINDOW_END_MS);
    printf("SNR = 10 * log10(tick_energy / noise_energy)\n");
    printf("Auto-Gain: %s (starts at %ddB, adjusts on overload)\n\n",
           g_auto_gain ? "ENABLED" : "disabled", gain);

    /* Set up signal handler */
    signal(SIGINT, signal_handler);

    /* Initialize GPS */
    printf("Connecting to GPS on %s...\n", gps_port);
    memset(&g_gps_ctx, 0, sizeof(g_gps_ctx));

    if (gps_open(&g_gps_ctx, gps_port, 115200) != 0) {
        fprintf(stderr, "Failed to connect to GPS\n");
        return 1;
    }

    /* Wait for GPS fix */
    gps_reading_t gps;
    bool have_fix = false;
    printf("Waiting for GPS fix...\n");

    for (int i = 0; i < 20 && g_running; i++) {
        if (gps_read_time(&g_gps_ctx, &gps, 1000) == 0 && gps.valid && gps.satellites >= 3) {
            printf("GPS fix: %s (SAT:%d)\n", gps.iso_string, gps.satellites);
            have_fix = true;
            break;
        }
        printf("  Waiting... (%d/20)\n", i + 1);
    }

    if (!have_fix) {
        fprintf(stderr, "Could not get GPS fix\n");
        gps_close(&g_gps_ctx);
        return 1;
    }

    /* Initialize decimator */
    printf("\nInitializing DSP...\n");
    decim_error_t derr = decim_create(&g_decimator, SAMPLE_RATE_HZ, DECIMATED_RATE_HZ);
    if (derr != DECIM_OK) {
        fprintf(stderr, "Failed to create decimator\n");
        gps_close(&g_gps_ctx);
        return 1;
    }

    /* Open SDR */
    printf("Opening SDR...\n");
    psdr_device_info_t devices[8];
    size_t num_devices = 0;

    psdr_error_t err = psdr_enumerate(devices, 8, &num_devices);
    if (err != PSDR_OK || num_devices == 0) {
        fprintf(stderr, "No SDR devices found\n");
        decim_destroy(g_decimator);
        gps_close(&g_gps_ctx);
        return 1;
    }

    psdr_context_t *ctx = NULL;
    err = psdr_open(&ctx, 0);
    if (err != PSDR_OK) {
        fprintf(stderr, "Failed to open SDR\n");
        decim_destroy(g_decimator);
        gps_close(&g_gps_ctx);
        return 1;
    }

    /* Interactive mode - skip scanning, go straight to live tuning */
    if (interactive) {
        int result = run_interactive_mode(ctx, gain, gps_port);
        psdr_close(ctx);
        decim_destroy(g_decimator);
        gps_close(&g_gps_ctx);
        return result;
    }

    /* Scan results */
    scan_result_t results[NUM_WWV_FREQS];
    memset(results, 0, sizeof(results));

    printf("\n===========================================\n");
    printf("Scanning %zu frequencies (%d sec each)\n", NUM_WWV_FREQS, scan_time);
    printf("===========================================\n");

    /* Scan each frequency */
    for (int i = 0; i < (int)NUM_WWV_FREQS && g_running; i++) {
        scan_frequency(ctx, WWV_FREQS_MHZ[i], scan_time, gain, &results[i]);
    }

    /* Print results table */
    printf("\n===========================================\n");
    printf("SCAN RESULTS\n");
    printf("===========================================\n\n");
    printf("%-8s  %-12s  %-12s  %-8s  %-10s\n",
           "FREQ", "TICK PWR", "NOISE PWR", "SNR", "TICKS");
    printf("%-8s  %-12s  %-12s  %-8s  %-10s\n",
           "------", "----------", "----------", "------", "--------");

    int best_idx = -1;
    double best_score = -1000.0;

    for (int i = 0; i < (int)NUM_WWV_FREQS; i++) {
        if (!results[i].scanned) {
            printf("%-8.1f  %-12s  %-12s  %-8s  %-10s\n",
                   WWV_FREQS_MHZ[i], "---", "---", "---", "---");
            continue;
        }

        char tick_str[16], noise_str[16], snr_str[16], ticks_str[16];
        snprintf(tick_str, sizeof(tick_str), "%.2e", results[i].tick_energy_avg);
        snprintf(noise_str, sizeof(noise_str), "%.2e", results[i].noise_energy_avg);
        snprintf(snr_str, sizeof(snr_str), "%+.1f dB", results[i].snr_db);
        snprintf(ticks_str, sizeof(ticks_str), "%d/%d",
                 results[i].ticks_detected, results[i].seconds_measured);

        /* Score: SNR + bonus for tick detection rate */
        double tick_rate = (results[i].seconds_measured > 0) ?
                           (double)results[i].ticks_detected / results[i].seconds_measured : 0.0;
        double score = results[i].snr_db + (tick_rate * 10.0);

        printf("%-8.1f  %-12s  %-12s  %-8s  %-10s",
               results[i].freq_mhz, tick_str, noise_str, snr_str, ticks_str);

        if (score > best_score && results[i].snr_db > 0.0) {
            best_score = score;
            best_idx = i;
        }

        if (i == best_idx) {
            printf("  <-- BEST");
        }
        printf("\n");
    }

    printf("\n");
    if (best_idx >= 0) {
        printf("===========================================\n");
        printf("RECOMMENDATION: %.1f MHz\n", results[best_idx].freq_mhz);
        printf("  SNR: %+.1f dB\n", results[best_idx].snr_db);
        printf("  Tick detection: %d/%d (%.0f%%)\n",
               results[best_idx].ticks_detected,
               results[best_idx].seconds_measured,
               100.0 * results[best_idx].ticks_detected / results[best_idx].seconds_measured);
        printf("\n  Record with:\n");
        printf("    phoenix_sdr -f %.1f -a Z -A -d <seconds> -o wwv_capture\n",
               results[best_idx].freq_mhz);
        printf("===========================================\n");
    } else {
        printf("No clear WWV signal detected on any frequency.\n");
        printf("Try:\n");
        printf("  - Different time of day (propagation changes)\n");
        printf("  - Check antenna/connections\n");
        printf("  - Lower gain (-g 30)\n");
    }

    /* Cleanup */
    psdr_close(ctx);
    decim_destroy(g_decimator);
    gps_close(&g_gps_ctx);

    if (g_dump_file) {
        fclose(g_dump_file);
        printf("Waveform data written to dump file\n");
    }

    printf("\nDone.\n");
    return 0;
}
