/**
 * @file wwv_tick_detect.c
 * @brief WWV/WWVH tick detector - honest version
 * 
 * Detects the 1000 Hz (WWV) and 1200 Hz (WWVH) tick tones in IQR recordings.
 * Prints raw sample numbers, energy levels, and timestamps for verification.
 * 
 * Usage: wwv_tick_detect <file.iqr>
 */

#include "iq_recorder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*============================================================================
 * Bandpass Filter for 1000 Hz (WWV tick tone)
 * 
 * Simple 2nd-order IIR bandpass, Q~5 (wider), center=1000Hz, Fs=48000
 * Widened to catch frequency offset
 *============================================================================*/

typedef struct {
    float x1, x2;  /* Input history */
    float y1, y2;  /* Output history */
} biquad_state_t;

/* Biquad coefficients for ~800-1200 Hz bandpass at 48kHz sample rate */
/* Wider Q to catch frequency variations */
static const float bp_b0 =  0.020083f;
static const float bp_b1 =  0.0f;
static const float bp_b2 = -0.020083f;
static const float bp_a1 = -1.959401f;
static const float bp_a2 =  0.959833f;

static float biquad_process(biquad_state_t *s, float x) {
    float y = bp_b0 * x + bp_b1 * s->x1 + bp_b2 * s->x2
                       - bp_a1 * s->y1 - bp_a2 * s->y2;
    s->x2 = s->x1;
    s->x1 = x;
    s->y2 = s->y1;
    s->y1 = y;
    return y;
}

/*============================================================================
 * Envelope follower (smoothed magnitude)
 *============================================================================*/

typedef struct {
    float level;
} envelope_state_t;

static float envelope_process(envelope_state_t *s, float x) {
    float mag = fabsf(x);
    /* Fast attack, medium decay for 5ms tick detection */
    if (mag > s->level) {
        s->level = s->level + 0.3f * (mag - s->level);  /* Very fast attack */
    } else {
        s->level = s->level + 0.01f * (mag - s->level); /* Medium decay */
    }
    return s->level;
}

/*============================================================================
 * Tick detector state
 *============================================================================*/

typedef struct {
    biquad_state_t bp;
    envelope_state_t env;
    
    float threshold;
    float noise_floor;
    int samples_above;
    int samples_below;
    uint64_t last_tick_sample;
    int tick_count;
    
    /* For adaptive threshold */
    float peak_level;
    float avg_level;
    uint64_t avg_count;
} tick_detector_t;

static void tick_detector_init(tick_detector_t *td) {
    memset(td, 0, sizeof(*td));
    td->threshold = 0.0f;  /* Will be set adaptively */
    td->noise_floor = 0.0f;
    td->last_tick_sample = 0;
    td->tick_count = 0;
    td->peak_level = 0.0f;
    td->avg_level = 0.0f;
    td->avg_count = 0;
}

/*============================================================================
 * Main
 *============================================================================*/

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("WWV/WWVH Tick Detector - Honest Version\n");
        printf("Usage: %s <file.iqr>\n\n", argv[0]);
        printf("Detects 1000 Hz tick tones and prints raw data for verification.\n");
        printf("Run on WWV recording: should see ticks ~1 second apart\n");
        printf("Run on random frequency: should see noise/garbage\n");
        return 1;
    }
    
    const char *filename = argv[1];
    iqr_reader_t *reader = NULL;
    iqr_error_t err;
    
    /* Open file */
    printf("Opening %s...\n", filename);
    err = iqr_open(&reader, filename);
    if (err != IQR_OK) {
        fprintf(stderr, "Failed to open: %s\n", iqr_strerror(err));
        return 1;
    }
    
    const iqr_header_t *hdr = iqr_get_header(reader);
    printf("\nFile: %s\n", filename);
    printf("Sample Rate: %.0f Hz\n", hdr->sample_rate_hz);
    printf("Center Freq: %.6f MHz\n", hdr->center_freq_hz / 1e6);
    printf("Duration: %.2f seconds\n", 
           (double)hdr->sample_count / hdr->sample_rate_hz);
    printf("\n");
    
    /* Initialize detector */
    tick_detector_t td;
    tick_detector_init(&td);
    
    /* First pass: analyze signal levels */
    printf("=== PASS 1: Analyzing signal levels ===\n");
    
    #define CHUNK_SIZE 4096
    int16_t xi[CHUNK_SIZE], xq[CHUNK_SIZE];
    uint32_t num_read;
    uint64_t sample_num = 0;
    
    float min_level = 1e9f, max_level = 0.0f;
    float sum_level = 0.0f;
    uint64_t level_count = 0;
    
    /* DC blocking filter state for AM demod */
    float dc_prev_in = 0.0f, dc_prev_out = 0.0f;
    
    while (1) {
        err = iqr_read(reader, xi, xq, CHUNK_SIZE, &num_read);
        if (err != IQR_OK || num_read == 0) break;
        
        for (uint32_t i = 0; i < num_read; i++) {
            /* AM demodulation: envelope = sqrt(I^2 + Q^2) */
            float fi = (float)xi[i];
            float fq = (float)xq[i];
            float envelope = sqrtf(fi*fi + fq*fq) / 32768.0f;
            
            /* DC blocking high-pass to remove carrier */
            float dc_out = envelope - dc_prev_in + 0.995f * dc_prev_out;
            dc_prev_in = envelope;
            dc_prev_out = dc_out;
            
            /* Now bandpass filter the audio for 1000 Hz */
            float filtered = biquad_process(&td.bp, dc_out);
            float level = envelope_process(&td.env, filtered);
            
            if (level > max_level) max_level = level;
            if (level < min_level && level > 0.0001f) min_level = level;
            sum_level += level;
            level_count++;
        }
        sample_num += num_read;
    }
    
    float avg_level = sum_level / (float)level_count;
    
    printf("1000 Hz band energy stats:\n");
    printf("  Min level:  %.6f\n", min_level);
    printf("  Max level:  %.6f\n", max_level);
    printf("  Avg level:  %.6f\n", avg_level);
    printf("  Ratio max/avg: %.2f\n", max_level / avg_level);
    printf("\n");
    
    /* Set threshold at 1.5x average (ticks should be above noise) */
    float threshold = avg_level * 1.5f;
    printf("Detection threshold: %.6f (1.5x average)\n", threshold);
    printf("\n");
    
    /* Reset for second pass */
    iqr_close(reader);
    err = iqr_open(&reader, filename);
    if (err != IQR_OK) {
        fprintf(stderr, "Failed to reopen: %s\n", iqr_strerror(err));
        return 1;
    }
    
    tick_detector_init(&td);
    sample_num = 0;
    
    /* Second pass: detect ticks */
    printf("=== PASS 2: Detecting ticks ===\n");
    printf("(Looking for 1000 Hz energy bursts above threshold)\n\n");
    printf("%-8s  %-12s  %-10s  %-10s  %-10s\n", 
           "TICK#", "SAMPLE", "TIME(s)", "LEVEL", "GAP(ms)");
    printf("%-8s  %-12s  %-10s  %-10s  %-10s\n",
           "------", "----------", "--------", "--------", "--------");
    
    int ticks_detected = 0;
    uint64_t last_tick_sample = 0;
    int in_tick = 0;
    int samples_in_tick = 0;
    float tick_peak_level = 0.0f;
    uint64_t tick_start_sample = 0;
    
    /* Minimum samples between ticks (debounce) - 0.5 seconds */
    uint64_t min_gap_samples = (uint64_t)(hdr->sample_rate_hz * 0.5);
    
    /* Expected tick duration: 5ms = 240 samples at 48kHz */
    int min_tick_samples = (int)(hdr->sample_rate_hz * 0.002);  /* 2ms min */
    int max_tick_samples = (int)(hdr->sample_rate_hz * 0.050);  /* 50ms max */
    
    /* DC blocking filter state for AM demod */
    float dc_prev_in2 = 0.0f, dc_prev_out2 = 0.0f;
    
    while (1) {
        err = iqr_read(reader, xi, xq, CHUNK_SIZE, &num_read);
        if (err != IQR_OK || num_read == 0) break;
        
        for (uint32_t i = 0; i < num_read; i++) {
            /* AM demodulation: envelope = sqrt(I^2 + Q^2) */
            float fi = (float)xi[i];
            float fq = (float)xq[i];
            float envelope = sqrtf(fi*fi + fq*fq) / 32768.0f;
            
            /* DC blocking high-pass to remove carrier */
            float dc_out = envelope - dc_prev_in2 + 0.995f * dc_prev_out2;
            dc_prev_in2 = envelope;
            dc_prev_out2 = dc_out;
            
            /* Now bandpass filter the audio for 1000 Hz */
            float filtered = biquad_process(&td.bp, dc_out);
            float level = envelope_process(&td.env, filtered);
            
            uint64_t current_sample = sample_num + i;
            
            if (!in_tick && level > threshold) {
                /* Rising edge - potential tick start */
                if (current_sample - last_tick_sample > min_gap_samples || last_tick_sample == 0) {
                    in_tick = 1;
                    samples_in_tick = 0;
                    tick_peak_level = level;
                    tick_start_sample = current_sample;
                }
            }
            
            if (in_tick) {
                samples_in_tick++;
                if (level > tick_peak_level) {
                    tick_peak_level = level;
                }
                
                if (level < threshold) {
                    /* Falling edge - tick ended */
                    in_tick = 0;
                    
                    /* Validate tick duration */
                    if (samples_in_tick >= min_tick_samples && 
                        samples_in_tick <= max_tick_samples) {
                        
                        ticks_detected++;
                        
                        double time_sec = (double)tick_start_sample / hdr->sample_rate_hz;
                        double gap_ms = 0.0;
                        if (last_tick_sample > 0) {
                            gap_ms = (double)(tick_start_sample - last_tick_sample) / 
                                     hdr->sample_rate_hz * 1000.0;
                        }
                        
                        printf("%-8d  %-12llu  %-10.3f  %-10.6f  %-10.1f\n",
                               ticks_detected,
                               (unsigned long long)tick_start_sample,
                               time_sec,
                               tick_peak_level,
                               gap_ms);
                        
                        last_tick_sample = tick_start_sample;
                    }
                }
            }
        }
        sample_num += num_read;
    }
    
    printf("\n=== SUMMARY ===\n");
    printf("Total ticks detected: %d\n", ticks_detected);
    printf("Recording duration: %.2f seconds\n", 
           (double)hdr->sample_count / hdr->sample_rate_hz);
    
    if (ticks_detected > 1) {
        printf("Expected ticks (1/sec): %.0f\n", 
               (double)hdr->sample_count / hdr->sample_rate_hz);
        printf("\nIf ticks are ~1000ms apart, this is likely WWV.\n");
        printf("If gap varies wildly or tick count is way off, it's noise.\n");
    } else {
        printf("\nFew/no ticks detected. Either:\n");
        printf("  - No WWV signal in recording\n");
        printf("  - Wrong frequency\n");
        printf("  - Signal too weak\n");
    }
    
    iqr_close(reader);
    return 0;
}
