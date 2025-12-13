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
#define TICK_WINDOW_END_MS      50      /* End of tick window (wider to catch timing jitter) */
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

/* Window boundaries in samples */
static int g_tick_start_sample;
static int g_tick_end_sample;
static int g_noise_start_sample;
static int g_noise_end_sample;
static int g_samples_per_second;

/* Energy accumulators */
static double g_tick_energy_sum = 0.0;
static int g_tick_energy_count = 0;
static double g_noise_energy_sum = 0.0;
static int g_noise_energy_count = 0;

/* Per-second results */
#define MAX_SECONDS 120
static double g_tick_energy[MAX_SECONDS];
static double g_noise_energy[MAX_SECONDS];
static int g_seconds_measured = 0;

/* DEBUG: Track signal levels through pipeline */
static float g_debug_max_raw = 0.0f;
static float g_debug_max_decim = 0.0f;
static float g_debug_max_mag = 0.0f;
static float g_debug_max_ac = 0.0f;
static float g_debug_max_filt = 0.0f;
static int g_debug_sample_count = 0;

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
    }
    
    if (!g_scanning) return;
    
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
        
        /* Determine which window this sample falls into */
        int sample_pos = g_samples_in_second;
        
        if (sample_pos >= g_tick_start_sample && sample_pos < g_tick_end_sample) {
            /* In tick window */
            g_tick_energy_sum += energy;
            g_tick_energy_count++;
        }
        else if (sample_pos >= g_noise_start_sample && sample_pos < g_noise_end_sample) {
            /* In noise window */
            g_noise_energy_sum += energy;
            g_noise_energy_count++;
        }
        
        g_samples_in_second++;
        
        /* Wrap at second boundary */
        if (g_samples_in_second >= g_samples_per_second) {
            g_samples_in_second = 0;
        }
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
    biquad_init_bp(&g_bp_1000hz, (float)DECIMATED_RATE_HZ, 1000.0f, 5.0f);  /* Q=5, wider filter */
    g_tick_energy_sum = 0.0;
    g_tick_energy_count = 0;
    g_noise_energy_sum = 0.0;
    g_noise_energy_count = 0;
    g_samples_in_second = 0;
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
                g_samples_in_second = MS_TO_SAMPLES(gps.millisecond);
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
                    
                    g_tick_energy[g_seconds_measured] = g_tick_energy_sum / g_tick_energy_count;
                    g_noise_energy[g_seconds_measured] = g_noise_energy_sum / g_noise_energy_count;
                    g_seconds_measured++;
                }
                
                /* Reset for next second */
                g_tick_energy_sum = 0.0;
                g_tick_energy_count = 0;
                g_noise_energy_sum = 0.0;
                g_noise_energy_count = 0;
                
                /* Re-sync sample counter to GPS */
                g_samples_in_second = MS_TO_SAMPLES(gps.millisecond);
                
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
    
    printf("\nDone.\n");
    return 0;
}
