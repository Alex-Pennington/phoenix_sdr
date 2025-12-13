/**
 * @file iqr_play.c
 * @brief Simple IQR file playback utility
 * 
 * Plays back .iqr files through Windows audio output for verification.
 * 
 * Usage: iqr_play <file.iqr>
 */

#include "iq_recorder.h"
#include "audio_monitor.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <stdbool.h>
#include <math.h>

#ifdef _WIN32
#include <Windows.h>
#define sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define sleep_ms(ms) usleep((ms) * 1000)
#endif

static volatile bool g_running = true;

static void signal_handler(int sig) {
    (void)sig;
    printf("\nInterrupt received, stopping...\n");
    g_running = false;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("IQR File Playback Utility\n");
        printf("Usage: %s <file.iqr>\n", argv[0]);
        return 1;
    }
    
    const char *filename = argv[1];
    iqr_reader_t *reader = NULL;
    audio_monitor_t *audio = NULL;
    iqr_error_t iqr_err;
    audio_error_t audio_err;
    
    signal(SIGINT, signal_handler);
    
    /* Open IQR file */
    printf("Opening %s...\n", filename);
    iqr_err = iqr_open(&reader, filename);
    if (iqr_err != IQR_OK) {
        fprintf(stderr, "Failed to open file: %s\n", iqr_strerror(iqr_err));
        return 1;
    }
    
    /* Get file info */
    const iqr_header_t *hdr = iqr_get_header(reader);
    printf("\nFile Info:\n");
    printf("  Sample Rate:  %.0f Hz\n", hdr->sample_rate_hz);
    printf("  Center Freq:  %.6f MHz\n", hdr->center_freq_hz / 1e6);
    printf("  Bandwidth:    %u kHz\n", hdr->bandwidth_khz);
    printf("  Samples:      %llu\n", (unsigned long long)hdr->sample_count);
    printf("  Duration:     %.2f seconds\n", 
           (double)hdr->sample_count / hdr->sample_rate_hz);
    printf("\n");
    
    /* Create audio output */
    printf("Initializing audio...\n");
    audio_err = audio_create(&audio, hdr->sample_rate_hz);
    if (audio_err != AUDIO_OK) {
        fprintf(stderr, "Failed to create audio: %s\n", audio_strerror(audio_err));
        iqr_close(reader);
        return 1;
    }
    
    audio_err = audio_start(audio);
    if (audio_err != AUDIO_OK) {
        fprintf(stderr, "Failed to start audio: %s\n", audio_strerror(audio_err));
        audio_destroy(audio);
        iqr_close(reader);
        return 1;
    }
    
    /* Playback loop */
    printf("Playing... (Ctrl+C to stop)\n\n");
    
    #define CHUNK_SIZE 4096
    int16_t xi[CHUNK_SIZE], xq[CHUNK_SIZE];
    uint32_t num_read;
    uint64_t total_played = 0;
    
    /* Simple DC-blocking high-pass filter state */
    float dc_prev_in = 0.0f;
    float dc_prev_out = 0.0f;
    
    /* Simple low-pass filter state for smoothing */
    float lp_prev = 0.0f;
    
    while (g_running) {
        iqr_err = iqr_read(reader, xi, xq, CHUNK_SIZE, &num_read);
        if (iqr_err != IQR_OK || num_read == 0) {
            break;  /* EOF or error */
        }
        
        /* Just play the I channel directly with gain and DC blocking */
        for (uint32_t i = 0; i < num_read; i++) {
            float sample = (float)xi[i];
            
            /* DC blocking high-pass */
            float hp_out = sample - dc_prev_in + 0.995f * dc_prev_out;
            dc_prev_in = sample;
            dc_prev_out = hp_out;
            
            /* Gentle low-pass to cut harsh highs (~8kHz cutoff) */
            lp_prev = lp_prev + 0.5f * (hp_out - lp_prev);
            
            /* Big gain */
            float out = lp_prev * 100.0f;
            if (out > 32767.0f) out = 32767.0f;
            if (out < -32768.0f) out = -32768.0f;
            xi[i] = (int16_t)out;
        }
        
        audio_write(audio, xi, xq, num_read);
        total_played += num_read;
        
        /* Progress */
        double played_sec = (double)total_played / hdr->sample_rate_hz;
        double total_sec = (double)hdr->sample_count / hdr->sample_rate_hz;
        printf("Playing: %.1f / %.1f sec\r", played_sec, total_sec);
        fflush(stdout);
    }
    
    printf("\n\nPlayback complete.\n");
    
    /* Cleanup */
    audio_stop(audio);
    audio_destroy(audio);
    iqr_close(reader);
    
    return 0;
}
