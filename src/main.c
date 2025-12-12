/**
 * @file main.c
 * @brief Phoenix SDR test harness - device enumeration, streaming, and I/Q recording
 */

#include "phoenix_sdr.h"
#include "iq_recorder.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <stdbool.h>

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

#define RECORD_FILENAME     "capture.iqr"
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
static iqr_recorder_t *g_recorder = NULL;
static double g_sample_rate = SAMPLE_RATE_HZ;

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
    }
    
    /* Record samples if recorder is active */
    if (g_recorder && iqr_is_recording(g_recorder)) {
        iqr_error_t err = iqr_write(g_recorder, xi, xq, count);
        if (err != IQR_OK) {
            fprintf(stderr, "Recording error: %s\n", iqr_strerror(err));
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
        printf("Recording: %.1f / %d sec  (%llu samples)\n", 
               duration, RECORD_DURATION_SEC,
               (unsigned long long)g_sample_count);
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
    psdr_device_info_t devices[8];
    size_t num_devices = 0;
    psdr_context_t *ctx = NULL;
    psdr_config_t config;
    
    printf("===========================================\n");
    printf("Phoenix SDR - I/Q Recording Test\n");
    printf("===========================================\n\n");
    
    /* Set up signal handler */
    signal(SIGINT, signal_handler);
    
    /* Enumerate devices */
    printf("Enumerating SDRplay devices...\n");
    err = psdr_enumerate(devices, 8, &num_devices);
    if (err != PSDR_OK) {
        fprintf(stderr, "Enumeration failed: %s\n", psdr_strerror(err));
        return 1;
    }
    
    printf("Found %zu device(s)\n\n", num_devices);
    
    if (num_devices == 0) {
        fprintf(stderr, "No devices found. Is an RSP connected?\n");
        return 1;
    }
    
    /* Open first device */
    printf("Opening device 0...\n");
    err = psdr_open(&ctx, 0);
    if (err != PSDR_OK) {
        fprintf(stderr, "Open failed: %s\n", psdr_strerror(err));
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
    printf("  Sample Rate:  %.3f MSPS\n", config.sample_rate_hz / 1e6);
    printf("  Bandwidth:    %d kHz\n", config.bandwidth);
    printf("  Gain Red:     %d dB\n", config.gain_reduction);
    printf("  LNA State:    %d\n", config.lna_state);
    printf("  Duration:     %d seconds\n", RECORD_DURATION_SEC);
    printf("  Output:       %s\n", RECORD_FILENAME);
    printf("\n");
    
    err = psdr_configure(ctx, &config);
    if (err != PSDR_OK) {
        fprintf(stderr, "Configure failed: %s\n", psdr_strerror(err));
        psdr_close(ctx);
        return 1;
    }
    
    /* Create I/Q recorder */
    iqr_err = iqr_create(&g_recorder, 0);
    if (iqr_err != IQR_OK) {
        fprintf(stderr, "Failed to create recorder: %s\n", iqr_strerror(iqr_err));
        psdr_close(ctx);
        return 1;
    }
    
    /* Start recording */
    iqr_err = iqr_start(g_recorder, 
                        RECORD_FILENAME,
                        config.sample_rate_hz,
                        config.freq_hz,
                        config.bandwidth,
                        config.gain_reduction,
                        config.lna_state);
    if (iqr_err != IQR_OK) {
        fprintf(stderr, "Failed to start recording: %s\n", iqr_strerror(iqr_err));
        iqr_destroy(g_recorder);
        psdr_close(ctx);
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
        iqr_stop(g_recorder);
        iqr_destroy(g_recorder);
        psdr_close(ctx);
        return 1;
    }
    
    /* Run until duration reached or interrupted */
    while (g_running && psdr_is_streaming(ctx)) {
        sleep_ms(100);
    }
    
    /* Stop streaming */
    printf("\nStopping stream...\n");
    psdr_stop(ctx);
    
    /* Stop recording */
    printf("Finalizing recording...\n");
    iqr_stop(g_recorder);
    
    /* Print final stats */
    printf("\n===========================================\n");
    printf("Recording Complete\n");
    printf("===========================================\n");
    printf("  Total samples: %llu\n", (unsigned long long)g_sample_count);
    printf("  Duration:      %.2f seconds\n", (double)g_sample_count / g_sample_rate);
    printf("  File:          %s\n", RECORD_FILENAME);
    printf("  File size:     %.2f MB\n", 
           (64.0 + g_sample_count * 4.0) / (1024.0 * 1024.0));
    
    /* Now test reading it back */
    printf("\n===========================================\n");
    printf("Verifying Recording\n");
    printf("===========================================\n");
    
    iqr_reader_t *reader = NULL;
    iqr_err = iqr_open(&reader, RECORD_FILENAME);
    if (iqr_err != IQR_OK) {
        fprintf(stderr, "Failed to open recording: %s\n", iqr_strerror(iqr_err));
    } else {
        const iqr_header_t *hdr = iqr_get_header(reader);
        printf("  Magic:         %.4s\n", hdr->magic);
        printf("  Version:       %u\n", hdr->version);
        printf("  Sample Rate:   %.0f Hz\n", hdr->sample_rate_hz);
        printf("  Center Freq:   %.0f Hz\n", hdr->center_freq_hz);
        printf("  Bandwidth:     %u kHz\n", hdr->bandwidth_khz);
        printf("  Gain Red:      %d dB\n", hdr->gain_reduction);
        printf("  LNA State:     %u\n", hdr->lna_state);
        printf("  Sample Count:  %llu\n", (unsigned long long)hdr->sample_count);
        printf("  Duration:      %.2f sec\n", 
               (double)hdr->sample_count / hdr->sample_rate_hz);
        
        /* Read first few samples to verify */
        int16_t xi[16], xq[16];
        uint32_t num_read;
        iqr_err = iqr_read(reader, xi, xq, 16, &num_read);
        if (iqr_err == IQR_OK && num_read > 0) {
            printf("\n  First 8 samples (I, Q):\n");
            for (int i = 0; i < 8 && i < (int)num_read; i++) {
                printf("    [%d]: %6d, %6d\n", i, xi[i], xq[i]);
            }
        }
        
        iqr_close(reader);
        printf("\nVerification OK!\n");
    }
    
    /* Cleanup */
    iqr_destroy(g_recorder);
    psdr_close(ctx);
    
    printf("\nDone.\n");
    return 0;
}
