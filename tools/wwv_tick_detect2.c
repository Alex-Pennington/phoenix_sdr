/**
 * @file wwv_tick_detect2.c
 * @brief WWV/WWVH tick detector - dual frequency version
 * 
 * Separates WWV (1000 Hz) from WWVH (1200 Hz) tick tones.
 * WWV ticks on the second, WWVH ticks 500ms later.
 * 
 * Usage: wwv_tick_detect2 <file.iqr>
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
 * Biquad Bandpass Filter
 *============================================================================*/

typedef struct {
    float b0, b1, b2;
    float a1, a2;
    float x1, x2;
    float y1, y2;
} biquad_t;

/* Design a 2nd-order bandpass filter */
static void biquad_design_bp(biquad_t *bq, float fs, float fc, float Q) {
    float w0 = 2.0f * M_PI * fc / fs;
    float alpha = sinf(w0) / (2.0f * Q);
    
    float b0 = alpha;
    float b1 = 0.0f;
    float b2 = -alpha;
    float a0 = 1.0f + alpha;
    float a1 = -2.0f * cosf(w0);
    float a2 = 1.0f - alpha;
    
    /* Normalize */
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

static void biquad_reset(biquad_t *bq) {
    bq->x1 = bq->x2 = 0.0f;
    bq->y1 = bq->y2 = 0.0f;
}

/*============================================================================
 * Envelope follower
 *============================================================================*/

typedef struct {
    float level;
} envelope_t;

static void envelope_init(envelope_t *env) {
    env->level = 0.0f;
}

static float envelope_process(envelope_t *env, float x) {
    float mag = fabsf(x);
    if (mag > env->level) {
        env->level += 0.3f * (mag - env->level);   /* Fast attack */
    } else {
        env->level += 0.01f * (mag - env->level);  /* Slow decay */
    }
    return env->level;
}

/*============================================================================
 * Channel detector (one per frequency)
 *============================================================================*/

typedef struct {
    const char *name;
    float center_freq;
    biquad_t bp;
    envelope_t env;
    
    /* Detection state */
    int in_tick;
    int samples_in_tick;
    float tick_peak;
    uint64_t tick_start;
    uint64_t last_tick;
    int tick_count;
    
    /* Level stats */
    float max_level;
    float sum_level;
    uint64_t level_count;
    float threshold;
} channel_t;

static void channel_init(channel_t *ch, const char *name, float fc, float fs) {
    ch->name = name;
    ch->center_freq = fc;
    biquad_design_bp(&ch->bp, fs, fc, 10.0f);  /* Q=10 for tight filter */
    envelope_init(&ch->env);
    
    ch->in_tick = 0;
    ch->samples_in_tick = 0;
    ch->tick_peak = 0.0f;
    ch->tick_start = 0;
    ch->last_tick = 0;
    ch->tick_count = 0;
    
    ch->max_level = 0.0f;
    ch->sum_level = 0.0f;
    ch->level_count = 0;
    ch->threshold = 0.0f;
}

static void channel_reset(channel_t *ch, float fs) {
    biquad_design_bp(&ch->bp, fs, ch->center_freq, 10.0f);
    envelope_init(&ch->env);
    ch->in_tick = 0;
    ch->samples_in_tick = 0;
    ch->tick_peak = 0.0f;
    ch->tick_start = 0;
    ch->last_tick = 0;
    ch->tick_count = 0;
}

/*============================================================================
 * Main
 *============================================================================*/

#define CHUNK_SIZE 4096

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("WWV/WWVH Tick Detector - Dual Frequency Version\n");
        printf("================================================\n");
        printf("Usage: %s <file.iqr>\n\n", argv[0]);
        printf("Separates WWV (1000 Hz, Colorado) from WWVH (1200 Hz, Hawaii).\n");
        printf("WWV ticks on the second, WWVH ticks 500ms later.\n");
        return 1;
    }
    
    const char *filename = argv[1];
    iqr_reader_t *reader = NULL;
    iqr_error_t err;
    
    /* Open file */
    err = iqr_open(&reader, filename);
    if (err != IQR_OK) {
        fprintf(stderr, "Failed to open: %s\n", iqr_strerror(err));
        return 1;
    }
    
    const iqr_header_t *hdr = iqr_get_header(reader);
    float fs = (float)hdr->sample_rate_hz;
    
    printf("\n");
    printf("File: %s\n", filename);
    printf("Sample Rate: %.0f Hz\n", fs);
    printf("Center Freq: %.6f MHz\n", hdr->center_freq_hz / 1e6);
    printf("Duration: %.2f seconds\n\n", 
           (double)hdr->sample_count / hdr->sample_rate_hz);
    
    /* Initialize channels */
    channel_t wwv, wwvh;
    channel_init(&wwv, "WWV", 1000.0f, fs);
    channel_init(&wwvh, "WWVH", 1200.0f, fs);
    
    int16_t xi[CHUNK_SIZE], xq[CHUNK_SIZE];
    uint32_t num_read;
    uint64_t sample_num = 0;
    
    /* DC blocking state */
    float dc_prev_in = 0.0f, dc_prev_out = 0.0f;
    
    /*========================================================================
     * Pass 1: Analyze levels for each channel
     *========================================================================*/
    printf("=== PASS 1: Analyzing signal levels ===\n\n");
    
    while (1) {
        err = iqr_read(reader, xi, xq, CHUNK_SIZE, &num_read);
        if (err != IQR_OK || num_read == 0) break;
        
        for (uint32_t i = 0; i < num_read; i++) {
            /* AM demod */
            float fi = (float)xi[i];
            float fq = (float)xq[i];
            float envelope = sqrtf(fi*fi + fq*fq) / 32768.0f;
            
            /* DC block */
            float audio = envelope - dc_prev_in + 0.995f * dc_prev_out;
            dc_prev_in = envelope;
            dc_prev_out = audio;
            
            /* Process each channel */
            float filt_wwv = biquad_process(&wwv.bp, audio);
            float level_wwv = envelope_process(&wwv.env, filt_wwv);
            wwv.sum_level += level_wwv;
            wwv.level_count++;
            if (level_wwv > wwv.max_level) wwv.max_level = level_wwv;
            
            float filt_wwvh = biquad_process(&wwvh.bp, audio);
            float level_wwvh = envelope_process(&wwvh.env, filt_wwvh);
            wwvh.sum_level += level_wwvh;
            wwvh.level_count++;
            if (level_wwvh > wwvh.max_level) wwvh.max_level = level_wwvh;
        }
        sample_num += num_read;
    }
    
    /* Calculate thresholds */
    float avg_wwv = wwv.sum_level / (float)wwv.level_count;
    float avg_wwvh = wwvh.sum_level / (float)wwvh.level_count;
    
    wwv.threshold = avg_wwv * 1.5f;
    wwvh.threshold = avg_wwvh * 1.5f;
    
    printf("WWV (1000 Hz):\n");
    printf("  Max level:  %.6f\n", wwv.max_level);
    printf("  Avg level:  %.6f\n", avg_wwv);
    printf("  Ratio:      %.2f\n", wwv.max_level / avg_wwv);
    printf("  Threshold:  %.6f\n\n", wwv.threshold);
    
    printf("WWVH (1200 Hz):\n");
    printf("  Max level:  %.6f\n", wwvh.max_level);
    printf("  Avg level:  %.6f\n", avg_wwvh);
    printf("  Ratio:      %.2f\n", wwvh.max_level / avg_wwvh);
    printf("  Threshold:  %.6f\n\n", wwvh.threshold);
    
    /*========================================================================
     * Pass 2: Detect ticks
     *========================================================================*/
    
    /* Reset for second pass */
    iqr_close(reader);
    err = iqr_open(&reader, filename);
    if (err != IQR_OK) {
        fprintf(stderr, "Failed to reopen: %s\n", iqr_strerror(err));
        return 1;
    }
    
    channel_reset(&wwv, fs);
    channel_reset(&wwvh, fs);
    sample_num = 0;
    dc_prev_in = dc_prev_out = 0.0f;
    
    /* Detection params */
    uint64_t min_gap = (uint64_t)(fs * 0.3);   /* 300ms min between ticks */
    int min_samples = (int)(fs * 0.002);       /* 2ms min duration */
    int max_samples = (int)(fs * 0.050);       /* 50ms max duration */
    
    printf("=== PASS 2: Detecting ticks ===\n\n");
    printf("%-6s  %-8s  %-12s  %-10s  %-10s  %-10s\n",
           "TICK#", "STATION", "SAMPLE", "TIME(s)", "LEVEL", "GAP(ms)");
    printf("%-6s  %-8s  %-12s  %-10s  %-10s  %-10s\n",
           "-----", "-------", "----------", "--------", "--------", "--------");
    
    int total_ticks = 0;
    
    while (1) {
        err = iqr_read(reader, xi, xq, CHUNK_SIZE, &num_read);
        if (err != IQR_OK || num_read == 0) break;
        
        for (uint32_t i = 0; i < num_read; i++) {
            uint64_t current = sample_num + i;
            
            /* AM demod */
            float fi = (float)xi[i];
            float fq = (float)xq[i];
            float envelope = sqrtf(fi*fi + fq*fq) / 32768.0f;
            
            /* DC block */
            float audio = envelope - dc_prev_in + 0.995f * dc_prev_out;
            dc_prev_in = envelope;
            dc_prev_out = audio;
            
            /* Process WWV channel */
            float filt_wwv = biquad_process(&wwv.bp, audio);
            float level_wwv = envelope_process(&wwv.env, filt_wwv);
            
            /* WWV tick detection */
            if (!wwv.in_tick && level_wwv > wwv.threshold) {
                if (current - wwv.last_tick > min_gap || wwv.last_tick == 0) {
                    wwv.in_tick = 1;
                    wwv.samples_in_tick = 0;
                    wwv.tick_peak = level_wwv;
                    wwv.tick_start = current;
                }
            }
            if (wwv.in_tick) {
                wwv.samples_in_tick++;
                if (level_wwv > wwv.tick_peak) wwv.tick_peak = level_wwv;
                
                if (level_wwv < wwv.threshold) {
                    wwv.in_tick = 0;
                    if (wwv.samples_in_tick >= min_samples && 
                        wwv.samples_in_tick <= max_samples) {
                        
                        wwv.tick_count++;
                        total_ticks++;
                        
                        double time_sec = (double)wwv.tick_start / fs;
                        double gap_ms = 0.0;
                        if (wwv.last_tick > 0) {
                            gap_ms = (double)(wwv.tick_start - wwv.last_tick) / fs * 1000.0;
                        }
                        
                        printf("%-6d  %-8s  %-12llu  %-10.3f  %-10.6f  %-10.1f\n",
                               total_ticks, "WWV",
                               (unsigned long long)wwv.tick_start,
                               time_sec, wwv.tick_peak, gap_ms);
                        
                        wwv.last_tick = wwv.tick_start;
                    }
                }
            }
            
            /* Process WWVH channel */
            float filt_wwvh = biquad_process(&wwvh.bp, audio);
            float level_wwvh = envelope_process(&wwvh.env, filt_wwvh);
            
            /* WWVH tick detection */
            if (!wwvh.in_tick && level_wwvh > wwvh.threshold) {
                if (current - wwvh.last_tick > min_gap || wwvh.last_tick == 0) {
                    wwvh.in_tick = 1;
                    wwvh.samples_in_tick = 0;
                    wwvh.tick_peak = level_wwvh;
                    wwvh.tick_start = current;
                }
            }
            if (wwvh.in_tick) {
                wwvh.samples_in_tick++;
                if (level_wwvh > wwvh.tick_peak) wwvh.tick_peak = level_wwvh;
                
                if (level_wwvh < wwvh.threshold) {
                    wwvh.in_tick = 0;
                    if (wwvh.samples_in_tick >= min_samples && 
                        wwvh.samples_in_tick <= max_samples) {
                        
                        wwvh.tick_count++;
                        total_ticks++;
                        
                        double time_sec = (double)wwvh.tick_start / fs;
                        double gap_ms = 0.0;
                        if (wwvh.last_tick > 0) {
                            gap_ms = (double)(wwvh.tick_start - wwvh.last_tick) / fs * 1000.0;
                        }
                        
                        printf("%-6d  %-8s  %-12llu  %-10.3f  %-10.6f  %-10.1f\n",
                               total_ticks, "WWVH",
                               (unsigned long long)wwvh.tick_start,
                               time_sec, wwvh.tick_peak, gap_ms);
                        
                        wwvh.last_tick = wwvh.tick_start;
                    }
                }
            }
        }
        sample_num += num_read;
    }
    
    /*========================================================================
     * Summary
     *========================================================================*/
    printf("\n=== SUMMARY ===\n");
    printf("Recording duration: %.2f seconds\n\n", 
           (double)hdr->sample_count / fs);
    printf("WWV  (1000 Hz, Colorado): %d ticks\n", wwv.tick_count);
    printf("WWVH (1200 Hz, Hawaii):   %d ticks\n", wwvh.tick_count);
    printf("Total:                    %d ticks\n\n", total_ticks);
    
    if (wwv.tick_count > 0 && wwvh.tick_count > 0) {
        printf("Both stations detected! WWV and WWVH are 500ms offset.\n");
    } else if (wwv.tick_count > 0) {
        printf("Only WWV (Colorado) detected.\n");
    } else if (wwvh.tick_count > 0) {
        printf("Only WWVH (Hawaii) detected.\n");
    } else {
        printf("No ticks detected. Check signal strength or frequency.\n");
    }
    
    iqr_close(reader);
    return 0;
}
