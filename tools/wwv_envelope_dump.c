/**
 * @file wwv_envelope_dump.c
 * @brief Dump 1000Hz envelope to CSV for analysis
 * 
 * Outputs time,envelope pairs so we can see the signal shape
 */

#include "iq_recorder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Biquad bandpass filter */
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
        printf("Usage: %s <file.iqr> [output.csv]\n", argv[0]);
        printf("Dumps 1000Hz envelope for analysis\n");
        printf("Output: time_sec, am_envelope, filtered_1000hz, smoothed\n");
        return 1;
    }
    
    const char *infile = argv[1];
    const char *outfile = (argc > 2) ? argv[2] : "envelope.csv";
    
    iqr_reader_t *reader = NULL;
    iqr_error_t err = iqr_open(&reader, infile);
    if (err != IQR_OK) {
        fprintf(stderr, "Failed to open %s: %s\n", infile, iqr_strerror(err));
        return 1;
    }
    
    const iqr_header_t *hdr = iqr_get_header(reader);
    double sample_rate = hdr->sample_rate_hz;
    
    printf("Input: %s\n", infile);
    printf("Sample Rate: %.0f Hz\n", sample_rate);
    printf("Duration: %.2f sec\n", (double)hdr->sample_count / sample_rate);
    printf("Output: %s\n", outfile);
    
    FILE *csv = fopen(outfile, "w");
    if (!csv) {
        fprintf(stderr, "Failed to create %s\n", outfile);
        iqr_close(reader);
        return 1;
    }
    
    fprintf(csv, "time_sec,am_envelope,filtered_1000hz,smoothed\n");
    
    /* Setup 1000 Hz bandpass - wider Q to catch variations */
    biquad_t bp;
    biquad_init_bp(&bp, (float)sample_rate, 1000.0f, 5.0f);  /* Q=5 for ~200Hz bandwidth */
    
    /* Smoothing for envelope */
    float smoothed = 0.0f;
    float smooth_alpha = 0.02f;  /* Smoothing factor */
    
    int16_t xi[1024], xq[1024];
    uint32_t num_read;
    uint64_t sample_num = 0;
    
    /* Downsample output - write every N samples */
    int downsample = (int)(sample_rate / 1000);  /* ~1000 points per second */
    if (downsample < 1) downsample = 1;
    int ds_count = 0;
    
    printf("Processing (writing every %d samples)...\n", downsample);
    
    while (iqr_read(reader, xi, xq, 1024, &num_read) == IQR_OK && num_read > 0) {
        for (uint32_t i = 0; i < num_read; i++) {
            /* AM demodulation: envelope = sqrt(I² + Q²) */
            float fi = (float)xi[i];
            float fq = (float)xq[i];
            float am_env = sqrtf(fi*fi + fq*fq) / 32768.0f;
            
            /* Bandpass filter around 1000 Hz */
            float filtered = biquad_process(&bp, am_env);
            float filtered_mag = fabsf(filtered);
            
            /* Smooth the filtered output */
            smoothed = smoothed + smooth_alpha * (filtered_mag - smoothed);
            
            /* Write downsampled */
            ds_count++;
            if (ds_count >= downsample) {
                double time_sec = (double)(sample_num + i) / sample_rate;
                fprintf(csv, "%.6f,%.6f,%.6f,%.6f\n", 
                        time_sec, am_env, filtered_mag, smoothed);
                ds_count = 0;
            }
        }
        sample_num += num_read;
    }
    
    fclose(csv);
    iqr_close(reader);
    
    printf("Done! Wrote %s\n", outfile);
    printf("\nTo analyze:\n");
    printf("  - Import CSV into Excel/Python/etc\n");
    printf("  - Plot 'smoothed' column vs time\n");
    printf("  - Look for 5ms spikes at 1-second intervals\n");
    printf("  - Minute marker at :59->:00 should show gap or different pattern\n");
    
    return 0;
}
