/*
 * wwv_analyze.c - Automated WWV signal analysis
 * Analyzes recording and reports findings, not raw data
 */

#include "iq_recorder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define ANALYSIS_WINDOW_MS 10

// Biquad filter for bandpass
typedef struct {
    float b0, b1, b2;
    float a1, a2;
    float x1, x2;
    float y1, y2;
} biquad_t;

static void biquad_design_bp(biquad_t *bq, float fs, float fc, float Q) {
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

static float biquad_process(biquad_t *bq, float x) {
    float y = bq->b0 * x + bq->b1 * bq->x1 + bq->b2 * bq->x2
            - bq->a1 * bq->y1 - bq->a2 * bq->y2;
    bq->x2 = bq->x1;
    bq->x1 = x;
    bq->y2 = bq->y1;
    bq->y1 = y;
    return y;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <file.iqr>\n", argv[0]);
        printf("Analyzes WWV recording and reports minute marker candidates\n");
        return 1;
    }
    
    iqr_reader_t *reader = NULL;
    iqr_error_t err = iqr_open(&reader, argv[1]);
    if (err != IQR_OK) {
        fprintf(stderr, "Failed to open %s: %s\n", argv[1], iqr_strerror(err));
        return 1;
    }
    
    const iqr_header_t *hdr = iqr_get_header(reader);
    double sample_rate = hdr->sample_rate_hz;
    uint64_t total_samples = hdr->sample_count;
    double duration = (double)total_samples / sample_rate;
    
    int samples_per_window = (int)(sample_rate * ANALYSIS_WINDOW_MS / 1000);
    int num_windows = (int)(duration * 1000 / ANALYSIS_WINDOW_MS);
    
    printf("=== WWV Signal Analysis ===\n");
    printf("File: %s\n", argv[1]);
    printf("Sample Rate: %.0f Hz\n", sample_rate);
    printf("Duration: %.1f seconds\n", duration);
    printf("Expected minute markers: %d\n", (int)(duration / 60) + 1);
    printf("Analysis windows: %d (at %d ms each)\n\n", num_windows, ANALYSIS_WINDOW_MS);
    
    // Allocate envelope array
    double *envelope = calloc(num_windows, sizeof(double));
    if (!envelope) {
        fprintf(stderr, "Failed to allocate envelope array\n");
        iqr_close(reader);
        return 1;
    }
    
    // Setup 1000 Hz bandpass filter
    biquad_t bp1000;
    biquad_design_bp(&bp1000, (float)sample_rate, 1000.0f, 20.0f);
    
    // Process samples
    int16_t xi[4096], xq[4096];
    uint32_t num_read;
    int window_idx = 0;
    double window_energy = 0;
    int samples_in_window = 0;
    
    printf("Processing samples...\n");
    
    while (iqr_read(reader, xi, xq, 4096, &num_read) == IQR_OK && num_read > 0) {
        for (uint32_t i = 0; i < num_read; i++) {
            // Convert to float and get magnitude
            float mag = sqrtf((float)xi[i] * xi[i] + (float)xq[i] * xq[i]) / 32768.0f;
            
            // Bandpass filter
            float filtered = biquad_process(&bp1000, mag);
            window_energy += fabsf(filtered);
            samples_in_window++;
            
            if (samples_in_window >= samples_per_window) {
                if (window_idx < num_windows) {
                    envelope[window_idx++] = window_energy / samples_in_window;
                }
                window_energy = 0;
                samples_in_window = 0;
            }
        }
    }
    
    printf("Processed %d windows\n\n", window_idx);
    num_windows = window_idx;  // Use actual count
    
    // Calculate statistics
    double sum = 0, min_val = 1e9, max_val = 0;
    for (int i = 0; i < num_windows; i++) {
        sum += envelope[i];
        if (envelope[i] < min_val) min_val = envelope[i];
        if (envelope[i] > max_val) max_val = envelope[i];
    }
    double avg = sum / num_windows;
    
    // Calculate stddev
    double variance = 0;
    for (int i = 0; i < num_windows; i++) {
        double diff = envelope[i] - avg;
        variance += diff * diff;
    }
    double stddev = sqrt(variance / num_windows);
    
    printf("1000 Hz Envelope Statistics:\n");
    printf("  Min: %.6f\n", min_val);
    printf("  Max: %.6f\n", max_val);
    printf("  Avg: %.6f\n", avg);
    printf("  StdDev: %.6f\n", stddev);
    printf("  Dynamic Range: %.1f dB\n\n", 20*log10(max_val/min_val));
    
    // Look for sustained rises (potential minute markers)
    // Try multiple thresholds
    double thresholds[] = {0.5, 1.0, 1.5, 2.0};
    int num_thresholds = sizeof(thresholds) / sizeof(thresholds[0]);
    
    for (int t = 0; t < num_thresholds; t++) {
        double rise_threshold = avg + thresholds[t] * stddev;
        
        printf("=== RISE Detection (threshold: avg + %.1f*stddev = %.6f) ===\n", 
               thresholds[t], rise_threshold);
        
        // Find candidates
        int num_candidates = 0;
        int in_rise = 0;
        int rise_start = 0;
        double rise_sum = 0;
        double rise_peak = 0;
        int rise_count = 0;
        
        double candidate_times[50];
        double candidate_durations[50];
        
        for (int i = 0; i < num_windows; i++) {
            if (envelope[i] > rise_threshold) {
                if (!in_rise) {
                    in_rise = 1;
                    rise_start = i;
                    rise_sum = 0;
                    rise_peak = 0;
                    rise_count = 0;
                }
                rise_sum += envelope[i];
                rise_count++;
                if (envelope[i] > rise_peak) rise_peak = envelope[i];
            } else {
                if (in_rise) {
                    int rise_duration_ms = rise_count * ANALYSIS_WINDOW_MS;
                    if (rise_duration_ms >= 300 && num_candidates < 50) {
                        candidate_times[num_candidates] = rise_start * ANALYSIS_WINDOW_MS / 1000.0;
                        candidate_durations[num_candidates] = rise_duration_ms;
                        num_candidates++;
                    }
                    in_rise = 0;
                }
            }
        }
        
        if (num_candidates == 0) {
            printf("  No candidates found.\n\n");
        } else {
            printf("  Found %d candidates:\n", num_candidates);
            for (int i = 0; i < num_candidates && i < 10; i++) {
                printf("    #%d: t=%.2f sec, duration=%d ms\n", 
                       i+1, candidate_times[i], (int)candidate_durations[i]);
            }
            if (num_candidates > 10) {
                printf("    ... and %d more\n", num_candidates - 10);
            }
            
            // Check periodicity
            if (num_candidates >= 2) {
                printf("  Intervals:\n");
                for (int i = 1; i < num_candidates && i < 6; i++) {
                    double interval = candidate_times[i] - candidate_times[i-1];
                    printf("    #%d to #%d: %.2f sec", i, i+1, interval);
                    if (fabs(interval - 60.0) < 3.0) printf(" <-- ~60 sec!");
                    printf("\n");
                }
            }
            printf("\n");
        }
    }
    
    // Look for sustained DROPS
    printf("=== DROP Detection (threshold: avg - 0.5*stddev = %.6f) ===\n", 
           avg - 0.5 * stddev);
    
    double drop_threshold = avg - 0.5 * stddev;
    int num_drops = 0;
    int in_drop = 0;
    int drop_start = 0;
    int drop_count = 0;
    
    double drop_times[50];
    double drop_durations[50];
    
    for (int i = 0; i < num_windows; i++) {
        if (envelope[i] < drop_threshold) {
            if (!in_drop) {
                in_drop = 1;
                drop_start = i;
                drop_count = 0;
            }
            drop_count++;
        } else {
            if (in_drop) {
                int drop_duration_ms = drop_count * ANALYSIS_WINDOW_MS;
                if (drop_duration_ms >= 500 && num_drops < 50) {
                    drop_times[num_drops] = drop_start * ANALYSIS_WINDOW_MS / 1000.0;
                    drop_durations[num_drops] = drop_duration_ms;
                    num_drops++;
                }
                in_drop = 0;
            }
        }
    }
    
    if (num_drops == 0) {
        printf("  No significant drops found.\n");
    } else {
        printf("  Found %d drops:\n", num_drops);
        for (int i = 0; i < num_drops && i < 10; i++) {
            printf("    #%d: t=%.2f sec, duration=%d ms\n", 
                   i+1, drop_times[i], (int)drop_durations[i]);
        }
        if (num_drops > 10) {
            printf("    ... and %d more\n", num_drops - 10);
        }
        
        // Check periodicity
        if (num_drops >= 2) {
            printf("  Intervals:\n");
            for (int i = 1; i < num_drops && i < 6; i++) {
                double interval = drop_times[i] - drop_times[i-1];
                printf("    #%d to #%d: %.2f sec", i, i+1, interval);
                if (fabs(interval - 60.0) < 3.0) printf(" <-- ~60 sec!");
                printf("\n");
            }
        }
    }
    
    free(envelope);
    iqr_close(reader);
    
    printf("\nAnalysis complete.\n");
    return 0;
}
