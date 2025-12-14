/**
 * @file wwv_debug.c
 * @brief WWV signal level debugger - outputs envelope data for analysis
 * 
 * Dumps the 1000Hz and 1200Hz band energy over time so we can see
 * what the detector is actually seeing.
 * 
 * Usage: wwv_debug <file.iqr> [start_sec] [duration_sec]
 */

#include "iq_recorder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    float b0, b1, b2;
    float a1, a2;
    float x1, x2;
    float y1, y2;
} biquad_t;

static void biquad_design_bp(biquad_t *bq, float fs, float fc, float Q) {
    float w0 = 2.0f * M_PI * fc / fs;
    float alpha = sinf(w0) / (2.0f * Q);
    
    float b0 = alpha;
    float b1 = 0.0f;
    float b2 = -alpha;
    float a0 = 1.0f + alpha;
    float a1 = -2.0f * cosf(w0);
    float a2 = 1.0f - alpha;
    
    bq->b0 = b0 / a0;
    bq->b1 = b1 / a0;
    bq->b2 = b2 / a0;
    bq->a1 = a1 / a0;
    bq->a2 = a2 / a0;
    
    bq->x1 = bq->x2 = 0.0f;
    bq->y1 = bq->y2 = 0.0f;
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

typedef struct {
    float level;
} envelope_t;

static float envelope_process(envelope_t *env, float x) {
    float mag = fabsf(x);
    if (mag > env->level) {
        env->level += 0.6f * (mag - env->level);   /* fast attack for 5ms tick */
    } else {
        env->level += 0.05f * (mag - env->level);  /* faster decay to see gaps */
    }
    return env->level;
}

#define CHUNK_SIZE 4096

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("WWV Signal Level Debugger\n");
        printf("Usage: %s <file.iqr> [start_sec] [duration_sec]\n", argv[0]);
        printf("\nOutputs time,wwv_level,wwvh_level every 1ms\n");
        printf("Pipe to file: wwv_debug file.iqr 0 5 > levels.csv\n");
        return 1;
    }
    
    const char *filename = argv[1];
    double start_sec = argc > 2 ? atof(argv[2]) : 0.0;
    double duration_sec = argc > 3 ? atof(argv[3]) : 5.0;
    
    iqr_reader_t *reader = NULL;
    iqr_error_t err;
    
    err = iqr_open(&reader, filename);
    if (err != IQR_OK) {
        fprintf(stderr, "Failed to open: %s\n", iqr_strerror(err));
        return 1;
    }
    
    const iqr_header_t *hdr = iqr_get_header(reader);
    float fs = (float)hdr->sample_rate_hz;
    
    fprintf(stderr, "File: %s\n", filename);
    fprintf(stderr, "Sample Rate: %.0f Hz\n", fs);
    fprintf(stderr, "Duration: %.2f sec\n", (double)hdr->sample_count / fs);
    fprintf(stderr, "Analyzing %.2f to %.2f sec\n\n", start_sec, start_sec + duration_sec);
    
    /* Seek to start position */
    uint64_t start_sample = (uint64_t)(start_sec * fs);
    uint64_t end_sample = (uint64_t)((start_sec + duration_sec) * fs);
    if (end_sample > hdr->sample_count) end_sample = hdr->sample_count;
    
    iqr_seek(reader, start_sample);
    
    /* Initialize filters - Q=2 for fast response to 5ms ticks */
    biquad_t bp_wwv, bp_wwvh;
    biquad_design_bp(&bp_wwv, fs, 1000.0f, 2.0f);
    biquad_design_bp(&bp_wwvh, fs, 1200.0f, 2.0f);
    
    envelope_t env_wwv = {0}, env_wwvh = {0};
    float dc_prev_in = 0.0f, dc_prev_out = 0.0f;
    
    /* First pass - get stats */
    int16_t xi[CHUNK_SIZE], xq[CHUNK_SIZE];
    uint32_t num_read;
    uint64_t sample_num = start_sample;
    
    float max_wwv = 0, max_wwvh = 0;
    float sum_wwv = 0, sum_wwvh = 0;
    uint64_t count = 0;
    
    while (sample_num < end_sample) {
        err = iqr_read(reader, xi, xq, CHUNK_SIZE, &num_read);
        if (err != IQR_OK || num_read == 0) break;
        
        for (uint32_t i = 0; i < num_read && sample_num + i < end_sample; i++) {
            float fi = (float)xi[i];
            float fq = (float)xq[i];
            float envelope = sqrtf(fi*fi + fq*fq) / 32768.0f;
            
            float audio = envelope - dc_prev_in + 0.995f * dc_prev_out;
            dc_prev_in = envelope;
            dc_prev_out = audio;
            
            float filt_wwv = biquad_process(&bp_wwv, audio);
            float level_wwv = envelope_process(&env_wwv, filt_wwv);
            
            float filt_wwvh = biquad_process(&bp_wwvh, audio);
            float level_wwvh = envelope_process(&env_wwvh, filt_wwvh);
            
            if (level_wwv > max_wwv) max_wwv = level_wwv;
            if (level_wwvh > max_wwvh) max_wwvh = level_wwvh;
            sum_wwv += level_wwv;
            sum_wwvh += level_wwvh;
            count++;
        }
        sample_num += num_read;
    }
    
    float avg_wwv = sum_wwv / count;
    float avg_wwvh = sum_wwvh / count;
    
    fprintf(stderr, "WWV (1000Hz):  max=%.6f avg=%.6f ratio=%.2f\n", max_wwv, avg_wwv, max_wwv/avg_wwv);
    fprintf(stderr, "WWVH (1200Hz): max=%.6f avg=%.6f ratio=%.2f\n", max_wwvh, avg_wwvh, max_wwvh/avg_wwvh);
    fprintf(stderr, "\nThresholds at 1.5x avg: WWV=%.6f WWVH=%.6f\n", avg_wwv*1.5f, avg_wwvh*1.5f);
    fprintf(stderr, "\n");
    
    /* Reset and second pass - output levels */
    iqr_seek(reader, start_sample);
    biquad_design_bp(&bp_wwv, fs, 1000.0f, 2.0f);
    biquad_design_bp(&bp_wwvh, fs, 1200.0f, 2.0f);
    env_wwv.level = 0;
    env_wwvh.level = 0;
    dc_prev_in = dc_prev_out = 0.0f;
    sample_num = start_sample;
    
    /* Output header */
    printf("time,wwv,wwvh,wwv_thresh,wwvh_thresh\n");
    
    /* Downsample output to every 1ms (48 samples at 48kHz) */
    int output_interval = (int)(fs * 0.001);
    int sample_counter = 0;
    float level_wwv = 0, level_wwvh = 0;
    
    while (sample_num < end_sample) {
        err = iqr_read(reader, xi, xq, CHUNK_SIZE, &num_read);
        if (err != IQR_OK || num_read == 0) break;
        
        for (uint32_t i = 0; i < num_read && sample_num + i < end_sample; i++) {
            float fi = (float)xi[i];
            float fq = (float)xq[i];
            float envelope = sqrtf(fi*fi + fq*fq) / 32768.0f;
            
            float audio = envelope - dc_prev_in + 0.995f * dc_prev_out;
            dc_prev_in = envelope;
            dc_prev_out = audio;
            
            float filt_wwv = biquad_process(&bp_wwv, audio);
            level_wwv = envelope_process(&env_wwv, filt_wwv);
            
            float filt_wwvh = biquad_process(&bp_wwvh, audio);
            level_wwvh = envelope_process(&env_wwvh, filt_wwvh);
            
            sample_counter++;
            if (sample_counter >= output_interval) {
                double time = (double)(sample_num + i) / fs;
                printf("%.5f,%.6f,%.6f,%.6f,%.6f\n", 
                       time, level_wwv, level_wwvh, avg_wwv*1.5f, avg_wwvh*1.5f);
                sample_counter = 0;
            }
        }
        sample_num += num_read;
    }
    
    iqr_close(reader);
    return 0;
}
