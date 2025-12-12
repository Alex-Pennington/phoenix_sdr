/**
 * @file main.c
 * @brief Phoenix SDR test harness - I/Q capture with decimation to 48kHz
 * 
 * Records raw I/Q at 2 MSPS and also outputs decimated 48kHz complex
 * baseband suitable for modem input.
 */

#include "phoenix_sdr.h"
#include "iq_recorder.h"
#include "decimator.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>

#ifdef _WIN32
#include <Windows.h>
#define sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define sleep_ms(ms) usleep((ms) * 1000)
#endif

/*============================================================================
 * Configuration
 *============================================================================*/

#define RAW_FILENAME        "capture_raw.iqr"    /* Full rate I/Q */
#define DECIM_FILENAME      "capture_48k.iqr"    /* Decimated I/Q */
#define RECORD_DURATION_SEC 5
#define CENTER_FREQ_HZ      7074000.0   /* 40m FT8 */
#define SAMPLE_RATE_HZ      2000000.0
#define BANDWIDTH_KHZ       200
#define GAIN_REDUCTION_DB   40
#define LNA_STATE           4

/*============================================================================
 * Globals
 *============================================================================*/

static volatile bool g_running = true;
static uint64_t g_sample_count = 0;
static uint32_t g_callback_count = 0;
static iqr_recorder_t *g_raw_recorder = NULL;
static iqr_recorder_t *g_decim_recorder = NULL;
static decim_state_t *g_decimator = NULL;
static double g_sample_rate = SAMPLE_RATE_HZ;

/* Decimator output buffer */
static decim_complex_t g_decim_buffer[8192];
static uint64_t g_decim_sample_count = 0;

/*============================================================================
 * Signal Handler
 *============================================================================*/

static void signal_handler(int sig) {
    (void)sig;
    printf("\nInterrupt received, stopping...\n");
    g_running = false;
}

/*============================================================================
 * Callbacks
 *============================================================================*/

static void on_samples(
    const int16_t *xi,
    const int16_t *xq,
    uint32_t count,
    bool reset,
    void *user_ctx
) {
    (void)user_ctx;
    
    if (reset) {
        printf("*** Stream reset - flush buffers ***\n");
        if (g_decimator) {
            decim_reset(g_decimator);
        }
    }
    
    /* Record raw samples if recorder is active */
    if (g_raw_recorder && iqr_is_recording(g_raw_recorder)) {
        iqr_error_t err = iqr_write(g_raw_recorder, xi, xq, count);
        if (err != IQR_OK) {
            fprintf(stderr, "Raw recording error: %s\n", iqr_strerror(err));
        }
    }
    
    /* Decimate and record 48kHz output */
    if (g_decimator && g_decim_recorder && iqr_is_recording(g_decim_recorder)) {
        size_t out_count = 0;
        decim_error_t derr = decim_process_int16(
            g_decimator, xi, xq, count,
            g_decim_buffer, sizeof(g_decim_buffer)/sizeof(g_decim_buffer[0]),
            &out_count
        );
        
        if (derr == DECIM_OK && out_count > 0) {
            /* Convert decim_complex_t to separate I/Q arrays for recorder */
            /* Note: iqr_write expects int16, so we scale back */
            int16_t *dec_i = malloc(out_count * sizeof(int16_t));
            int16_t *dec_q = malloc(out_count * sizeof(int16_t));
            
            if (dec_i && dec_q) {
                for (size_t i = 0; i < out_count; i++) {
                    /* Clamp to int16 range */
                    float fi = g_decim_buffer[i].i * 32767.0f;
                    float fq = g_decim_buffer[i].q * 32767.0f;
                    if (fi > 32767.0f) fi = 32767.0f;
                    if (fi < -32768.0f) fi = -32768.0f;
                    if (fq > 32767.0f) fq = 32767.0f;
                    if (fq < -32768.0f) fq = -32768.0f;
                    dec_i[i] = (int16_t)fi;
                    dec_q[i] = (int16_t)fq;
                }
                
                iqr_write(g_decim_recorder, dec_i, dec_q, (uint32_t)out_count);
                g_decim_sample_count += out_count;
            }
            
            free(dec_i);
            free(dec_q);
        }
    }
    
    g_sample_count += count;
    g_callback_count++;
    
    /* Check if we've recorded enough */
    double duration = (double)g_sample_count / g_sample_rate;
    if (duration >= RECORD_DURATION_SEC) {
        g_running = false;
    }
    
    /* Print progress every ~0.5 seconds */
    if (g_callback_count % 50 == 0) {
        printf("Recording: %.1f / %d sec  (raw: %llu, decim: %llu samples)\n", 
               duration, RECORD_DURATION_SEC,
               (unsigned long long)g_sample_count,
               (unsigned long long)g_decim_sample_count);
    }
}

static void on_gain_change(double gain_db, int lna_db, void *user_ctx) {
    (void)user_ctx;
    printf("Gain changed: %.2f dB (LNA: %d dB)\n", gain_db, lna_db);
}

static void on_overload(bool overloaded, void *user_ctx) {
    (void)user_ctx;
    if (overloaded) {
        printf("!!! ADC OVERLOAD - reduce gain !!!\n");
    } else {
        printf("ADC overload cleared\n");
    }
}

/*============================================================================
 * Main
 *============================================================================*/

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    
    psdr_error_t err;
    iqr_error_t iqr_err;
    decim_error_t decim_err;
    psdr_device_info_t devices[8];
    size_t num_devices = 0;
    psdr_context_t *ctx = NULL;
    psdr_config_t config;
    
    printf("===========================================\n");
    printf("Phoenix SDR - I/Q Recording with Decimation\n");
    printf("===========================================\n\n");
    
    /* Set up signal handler */
    signal(SIGINT, signal_handler);
    
    /* Create decimator */
    printf("Initializing decimator (2 MSPS → 48 kHz)...\n");
    decim_err = decim_create(&g_decimator, SAMPLE_RATE_HZ, 48000.0);
    if (decim_err != DECIM_OK) {
        fprintf(stderr, "Failed to create decimator: %s\n", decim_strerror(decim_err));
        return 1;
    }
    printf("Decimation ratio: %.2f\n\n", decim_get_ratio(g_decimator));
    
    /* Enumerate devices */
    printf("Enumerating SDRplay devices...\n");
    err = psdr_enumerate(devices, 8, &num_devices);
    if (err != PSDR_OK) {
        fprintf(stderr, "Enumeration failed: %s\n", psdr_strerror(err));
        decim_destroy(g_decimator);
        return 1;
    }
    
    printf("Found %zu device(s)\n\n", num_devices);
    
    if (num_devices == 0) {
        fprintf(stderr, "No devices found. Is an RSP connected?\n");
        decim_destroy(g_decimator);
        return 1;
    }
    
    /* Open first device */
    printf("Opening device 0...\n");
    err = psdr_open(&ctx, 0);
    if (err != PSDR_OK) {
        fprintf(stderr, "Open failed: %s\n", psdr_strerror(err));
        decim_destroy(g_decimator);
        return 1;
    }
    
    /* Configure for HF narrowband */
    psdr_config_defaults(&config);
    config.freq_hz = CENTER_FREQ_HZ;
    config.sample_rate_hz = SAMPLE_RATE_HZ;
    config.bandwidth = BANDWIDTH_KHZ;
    config.agc_mode = PSDR_AGC_DISABLED;
    config.gain_reduction = GAIN_REDUCTION_DB;
    config.lna_state = LNA_STATE;
    
    g_sample_rate = config.sample_rate_hz;
    
    printf("Configuration:\n");
    printf("  Frequency:    %.3f MHz\n", config.freq_hz / 1e6);
    printf("  Sample Rate:  %.3f MSPS (raw)\n", config.sample_rate_hz / 1e6);
    printf("  Output Rate:  48.000 kHz (decimated)\n");
    printf("  Bandwidth:    %d kHz\n", config.bandwidth);
    printf("  Gain Red:     %d dB\n", config.gain_reduction);
    printf("  LNA State:    %d\n", config.lna_state);
    printf("  Duration:     %d seconds\n", RECORD_DURATION_SEC);
    printf("  Raw output:   %s\n", RAW_FILENAME);
    printf("  Decim output: %s\n", DECIM_FILENAME);
    printf("\n");
    
    err = psdr_configure(ctx, &config);
    if (err != PSDR_OK) {
        fprintf(stderr, "Configure failed: %s\n", psdr_strerror(err));
        psdr_close(ctx);
        decim_destroy(g_decimator);
        return 1;
    }
    
    /* Create raw I/Q recorder */
    iqr_err = iqr_create(&g_raw_recorder, 0);
    if (iqr_err != IQR_OK) {
        fprintf(stderr, "Failed to create raw recorder: %s\n", iqr_strerror(iqr_err));
        psdr_close(ctx);
        decim_destroy(g_decimator);
        return 1;
    }
    
    /* Create decimated I/Q recorder */
    iqr_err = iqr_create(&g_decim_recorder, 0);
    if (iqr_err != IQR_OK) {
        fprintf(stderr, "Failed to create decim recorder: %s\n", iqr_strerror(iqr_err));
        iqr_destroy(g_raw_recorder);
        psdr_close(ctx);
        decim_destroy(g_decimator);
        return 1;
    }
    
    /* Start raw recording */
    iqr_err = iqr_start(g_raw_recorder, 
                        RAW_FILENAME,
                        config.sample_rate_hz,
                        config.freq_hz,
                        config.bandwidth,
                        config.gain_reduction,
                        config.lna_state);
    if (iqr_err != IQR_OK) {
        fprintf(stderr, "Failed to start raw recording: %s\n", iqr_strerror(iqr_err));
        iqr_destroy(g_raw_recorder);
        iqr_destroy(g_decim_recorder);
        psdr_close(ctx);
        decim_destroy(g_decimator);
        return 1;
    }
    
    /* Start decimated recording */
    iqr_err = iqr_start(g_decim_recorder, 
                        DECIM_FILENAME,
                        48000.0,              /* Decimated rate */
                        config.freq_hz,
                        config.bandwidth,
                        config.gain_reduction,
                        config.lna_state);
    if (iqr_err != IQR_OK) {
        fprintf(stderr, "Failed to start decim recording: %s\n", iqr_strerror(iqr_err));
        iqr_stop(g_raw_recorder);
        iqr_destroy(g_raw_recorder);
        iqr_destroy(g_decim_recorder);
        psdr_close(ctx);
        decim_destroy(g_decimator);
        return 1;
    }
    
    /* Set up callbacks */
    psdr_callbacks_t callbacks = {
        .on_samples = on_samples,
        .on_gain_change = on_gain_change,
        .on_overload = on_overload,
        .user_ctx = NULL
    };
    
    /* Start streaming */
    printf("Recording %d seconds of I/Q data...\n\n", RECORD_DURATION_SEC);
    err = psdr_start(ctx, &callbacks);
    if (err != PSDR_OK) {
        fprintf(stderr, "Start failed: %s\n", psdr_strerror(err));
        iqr_stop(g_raw_recorder);
        iqr_stop(g_decim_recorder);
        iqr_destroy(g_raw_recorder);
        iqr_destroy(g_decim_recorder);
        psdr_close(ctx);
        decim_destroy(g_decimator);
        return 1;
    }
    
    /* Run until duration reached or interrupted */
    while (g_running && psdr_is_streaming(ctx)) {
        sleep_ms(100);
    }
    
    /* Stop streaming */
    printf("\nStopping stream...\n");
    psdr_stop(ctx);
    
    /* Stop recordings */
    printf("Finalizing recordings...\n");
    iqr_stop(g_raw_recorder);
    iqr_stop(g_decim_recorder);
    
    /* Print final stats */
    printf("\n===========================================\n");
    printf("Recording Complete\n");
    printf("===========================================\n");
    printf("\nRaw recording (2 MSPS):\n");
    printf("  File:          %s\n", RAW_FILENAME);
    printf("  Samples:       %llu\n", (unsigned long long)g_sample_count);
    printf("  Duration:      %.2f seconds\n", (double)g_sample_count / g_sample_rate);
    printf("  Size:          %.2f MB\n", 
           (64.0 + g_sample_count * 4.0) / (1024.0 * 1024.0));
    
    printf("\nDecimated recording (48 kHz):\n");
    printf("  File:          %s\n", DECIM_FILENAME);
    printf("  Samples:       %llu\n", (unsigned long long)g_decim_sample_count);
    printf("  Duration:      %.2f seconds\n", (double)g_decim_sample_count / 48000.0);
    printf("  Size:          %.2f MB\n", 
           (64.0 + g_decim_sample_count * 4.0) / (1024.0 * 1024.0));
    
    printf("\nDecimation ratio achieved: %.2f:1\n",
           (double)g_sample_count / (double)g_decim_sample_count);
    
    /* Cleanup */
    iqr_destroy(g_raw_recorder);
    iqr_destroy(g_decim_recorder);
    decim_destroy(g_decimator);
    psdr_close(ctx);
    
    printf("\nDone.\n");
    return 0;
}
