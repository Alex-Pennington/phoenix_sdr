/**
 * @file wwv_analyze.c
 * @brief WWV/WWVH detailed timing analysis
 * 
 * Outputs detailed tick timing for analysis and refinement.
 * Identifies minute boundaries by looking for missing 29th/59th ticks.
 * 
 * Usage: wwv_analyze <file.iqr>
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
        env->level += 0.3f * (mag - env->level);
    } else {
        env->level += 0.01f * (mag - env->level);
    }
    return env->level;
}

/*============================================================================
 * Tick storage
 *============================================================================*/

#define MAX_TICKS 2000

typedef struct {
    uint64_t sample;
    double time_sec;
    float level;
    int station;  /* 0=WWV, 1=WWVH */
} tick_t;

static tick_t g_ticks[MAX_TICKS];
static int g_tick_count = 0;

/*============================================================================
 * Channel
 *============================================================================*/

typedef struct {
    const char *name;
    int id;
    float center_freq;
    biquad_t bp;
    envelope_t env;
    float threshold;
    
    int in_tick;
    int samples_in_tick;
    float tick_peak;
    uint64_t tick_start;
    uint64_t last_tick;
    int tick_count;
    
    float max_level;
    float sum_level;
    uint64_t level_count;
} channel_t;

static void channel_init(channel_t *ch, const char *name, int id, float fc, float fs) {
    ch->name = name;
    ch->id = id;
    ch->center_freq = fc;
    biquad_design_bp(&ch->bp, fs, fc, 10.0f);
    envelope_init(&ch->env);
    ch->threshold = 0.0f;
    
    ch->in_tick = 0;
    ch->samples_in_tick = 0;
    ch->tick_peak = 0.0f;
    ch->tick_start = 0;
    ch->last_tick = 0;
    ch->tick_count = 0;
    
    ch->max_level = 0.0f;
    ch->sum_level = 0.0f;
    ch->level_count = 0;
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
        printf("WWV/WWVH Timing Analyzer\n");
        printf("Usage: %s <file.iqr>\n", argv[0]);
        return 1;
    }
    
    const char *filename = argv[1];
    iqr_reader_t *reader = NULL;
    iqr_error_t err;
    
    err = iqr_open(&reader, filename);
    if (err != IQR_OK) {
        fprintf(stderr, "Failed to open: %s\n", iqr_strerror(err));
        return 1;
    }
    
    const iqr_header_t *hdr = iqr_get_header(reader);
    float fs = (float)hdr->sample_rate_hz;
    
    printf("\nFile: %s\n", filename);
    printf("Sample Rate: %.0f Hz\n", fs);
    printf("Duration: %.2f seconds\n\n", 
           (double)hdr->sample_count / fs);
    
    channel_t wwv, wwvh;
    channel_init(&wwv, "WWV", 0, 1000.0f, fs);
    channel_init(&wwvh, "WWVH", 1, 1200.0f, fs);
    
    int16_t xi[CHUNK_SIZE], xq[CHUNK_SIZE];
    uint32_t num_read;
    uint64_t sample_num = 0;
    float dc_prev_in = 0.0f, dc_prev_out = 0.0f;
    
    /* Pass 1: Get levels */
    printf("Pass 1: Analyzing levels...\n");
    
    while (1) {
        err = iqr_read(reader, xi, xq, CHUNK_SIZE, &num_read);
        if (err != IQR_OK || num_read == 0) break;
        
        for (uint32_t i = 0; i < num_read; i++) {
            float fi = (float)xi[i];
            float fq = (float)xq[i];
            float envelope = sqrtf(fi*fi + fq*fq) / 32768.0f;
            
            float audio = envelope - dc_prev_in + 0.995f * dc_prev_out;
            dc_prev_in = envelope;
            dc_prev_out = audio;
            
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
    
    float avg_wwv = wwv.sum_level / (float)wwv.level_count;
    float avg_wwvh = wwvh.sum_level / (float)wwvh.level_count;
    
    /* Use 2x average as threshold for better detection */
    wwv.threshold = avg_wwv * 2.0f;
    wwvh.threshold = avg_wwvh * 2.0f;
    
    printf("WWV:  max=%.6f avg=%.6f ratio=%.2f thresh=%.6f\n", 
           wwv.max_level, avg_wwv, wwv.max_level/avg_wwv, wwv.threshold);
    printf("WWVH: max=%.6f avg=%.6f ratio=%.2f thresh=%.6f\n\n",
           wwvh.max_level, avg_wwvh, wwvh.max_level/avg_wwvh, wwvh.threshold);
    
    /* Pass 2: Detect and store ticks */
    iqr_close(reader);
    err = iqr_open(&reader, filename);
    if (err != IQR_OK) return 1;
    
    channel_reset(&wwv, fs);
    channel_reset(&wwvh, fs);
    sample_num = 0;
    dc_prev_in = dc_prev_out = 0.0f;
    g_tick_count = 0;
    
    uint64_t min_gap = (uint64_t)(fs * 0.3);
    int min_samples = (int)(fs * 0.002);
    int max_samples = (int)(fs * 0.050);
    
    printf("Pass 2: Detecting ticks...\n\n");
    
    while (1) {
        err = iqr_read(reader, xi, xq, CHUNK_SIZE, &num_read);
        if (err != IQR_OK || num_read == 0) break;
        
        for (uint32_t i = 0; i < num_read; i++) {
            uint64_t current = sample_num + i;
            
            float fi = (float)xi[i];
            float fq = (float)xq[i];
            float envelope = sqrtf(fi*fi + fq*fq) / 32768.0f;
            
            float audio = envelope - dc_prev_in + 0.995f * dc_prev_out;
            dc_prev_in = envelope;
            dc_prev_out = audio;
            
            /* WWV */
            float filt_wwv = biquad_process(&wwv.bp, audio);
            float level_wwv = envelope_process(&wwv.env, filt_wwv);
            
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
                        wwv.samples_in_tick <= max_samples &&
                        g_tick_count < MAX_TICKS) {
                        
                        g_ticks[g_tick_count].sample = wwv.tick_start;
                        g_ticks[g_tick_count].time_sec = (double)wwv.tick_start / fs;
                        g_ticks[g_tick_count].level = wwv.tick_peak;
                        g_ticks[g_tick_count].station = 0;
                        g_tick_count++;
                        
                        wwv.tick_count++;
                        wwv.last_tick = wwv.tick_start;
                    }
                }
            }
            
            /* WWVH */
            float filt_wwvh = biquad_process(&wwvh.bp, audio);
            float level_wwvh = envelope_process(&wwvh.env, filt_wwvh);
            
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
                        wwvh.samples_in_tick <= max_samples &&
                        g_tick_count < MAX_TICKS) {
                        
                        g_ticks[g_tick_count].sample = wwvh.tick_start;
                        g_ticks[g_tick_count].time_sec = (double)wwvh.tick_start / fs;
                        g_ticks[g_tick_count].level = wwvh.tick_peak;
                        g_ticks[g_tick_count].station = 1;
                        g_tick_count++;
                        
                        wwvh.tick_count++;
                        wwvh.last_tick = wwvh.tick_start;
                    }
                }
            }
        }
        sample_num += num_read;
    }
    
    /* Sort ticks by time */
    for (int i = 0; i < g_tick_count - 1; i++) {
        for (int j = i + 1; j < g_tick_count; j++) {
            if (g_ticks[j].sample < g_ticks[i].sample) {
                tick_t tmp = g_ticks[i];
                g_ticks[i] = g_ticks[j];
                g_ticks[j] = tmp;
            }
        }
    }
    
    /* Print sorted tick list with gaps */
    printf("=== TICK TIMING (sorted by time) ===\n\n");
    printf("%-6s  %-6s  %-10s  %-10s  %-10s  %-6s\n",
           "#", "STA", "TIME(s)", "LEVEL", "GAP(ms)", "NOTE");
    printf("%-6s  %-6s  %-10s  %-10s  %-10s  %-6s\n",
           "-----", "-----", "--------", "--------", "--------", "------");
    
    double last_wwv_time = -10.0;
    double last_wwvh_time = -10.0;
    int wwv_seq = 0, wwvh_seq = 0;
    
    for (int i = 0; i < g_tick_count; i++) {
        const char *sta = g_ticks[i].station == 0 ? "WWV" : "WWVH";
        double gap_ms = 0.0;
        const char *note = "";
        
        if (g_ticks[i].station == 0) {
            gap_ms = (g_ticks[i].time_sec - last_wwv_time) * 1000.0;
            last_wwv_time = g_ticks[i].time_sec;
            wwv_seq++;
            
            /* Check for minute marker (2 second gap = missing 29th or 59th) */
            if (gap_ms > 1800.0 && gap_ms < 2200.0) {
                note = "<-MIN?";
            } else if (gap_ms > 2800.0 && gap_ms < 3200.0) {
                note = "<-2SEC";
            }
        } else {
            gap_ms = (g_ticks[i].time_sec - last_wwvh_time) * 1000.0;
            last_wwvh_time = g_ticks[i].time_sec;
            wwvh_seq++;
            
            if (gap_ms > 1800.0 && gap_ms < 2200.0) {
                note = "<-MIN?";
            } else if (gap_ms > 2800.0 && gap_ms < 3200.0) {
                note = "<-2SEC";
            }
        }
        
        /* First tick of each station shows large gap - ignore */
        if ((g_ticks[i].station == 0 && wwv_seq == 1) ||
            (g_ticks[i].station == 1 && wwvh_seq == 1)) {
            gap_ms = 0.0;
        }
        
        printf("%-6d  %-6s  %-10.3f  %-10.6f  %-10.1f  %s\n",
               i + 1, sta, g_ticks[i].time_sec, g_ticks[i].level, gap_ms, note);
    }
    
    /* Statistics */
    printf("\n=== GAP STATISTICS ===\n\n");
    
    /* Analyze WWV gaps */
    int wwv_gaps[10] = {0};  /* buckets: 0-500, 500-1000, 1000-1500, etc */
    double wwv_gap_sum = 0.0;
    int wwv_gap_count = 0;
    double prev_wwv = -10.0;
    
    for (int i = 0; i < g_tick_count; i++) {
        if (g_ticks[i].station == 0) {
            if (prev_wwv >= 0) {
                double gap = (g_ticks[i].time_sec - prev_wwv) * 1000.0;
                wwv_gap_sum += gap;
                wwv_gap_count++;
                int bucket = (int)(gap / 500.0);
                if (bucket >= 0 && bucket < 10) wwv_gaps[bucket]++;
            }
            prev_wwv = g_ticks[i].time_sec;
        }
    }
    
    printf("WWV gap distribution:\n");
    printf("  0-500ms:    %d\n", wwv_gaps[0]);
    printf("  500-1000ms: %d\n", wwv_gaps[1]);
    printf("  1000-1500ms: %d  (normal)\n", wwv_gaps[2]);
    printf("  1500-2000ms: %d\n", wwv_gaps[3]);
    printf("  2000-2500ms: %d  (minute marker?)\n", wwv_gaps[4]);
    printf("  2500-3000ms: %d\n", wwv_gaps[5]);
    printf("  3000+ms:    %d\n", wwv_gaps[6] + wwv_gaps[7] + wwv_gaps[8] + wwv_gaps[9]);
    if (wwv_gap_count > 0) {
        printf("  Average gap: %.1f ms\n", wwv_gap_sum / wwv_gap_count);
    }
    
    /* Analyze WWVH gaps */
    int wwvh_gaps[10] = {0};
    double wwvh_gap_sum = 0.0;
    int wwvh_gap_count = 0;
    double prev_wwvh = -10.0;
    
    for (int i = 0; i < g_tick_count; i++) {
        if (g_ticks[i].station == 1) {
            if (prev_wwvh >= 0) {
                double gap = (g_ticks[i].time_sec - prev_wwvh) * 1000.0;
                wwvh_gap_sum += gap;
                wwvh_gap_count++;
                int bucket = (int)(gap / 500.0);
                if (bucket >= 0 && bucket < 10) wwvh_gaps[bucket]++;
            }
            prev_wwvh = g_ticks[i].time_sec;
        }
    }
    
    printf("\nWWVH gap distribution:\n");
    printf("  0-500ms:    %d\n", wwvh_gaps[0]);
    printf("  500-1000ms: %d\n", wwvh_gaps[1]);
    printf("  1000-1500ms: %d  (normal)\n", wwvh_gaps[2]);
    printf("  1500-2000ms: %d\n", wwvh_gaps[3]);
    printf("  2000-2500ms: %d  (minute marker?)\n", wwvh_gaps[4]);
    printf("  2500-3000ms: %d\n", wwvh_gaps[5]);
    printf("  3000+ms:    %d\n", wwvh_gaps[6] + wwvh_gaps[7] + wwvh_gaps[8] + wwvh_gaps[9]);
    if (wwvh_gap_count > 0) {
        printf("  Average gap: %.1f ms\n", wwvh_gap_sum / wwvh_gap_count);
    }
    
    /* Summary */
    printf("\n=== SUMMARY ===\n");
    printf("WWV ticks:  %d\n", wwv.tick_count);
    printf("WWVH ticks: %d\n", wwvh.tick_count);
    printf("Total:      %d\n", g_tick_count);
    printf("Duration:   %.2f sec\n", (double)hdr->sample_count / fs);
    printf("Expected:   ~%.0f per station (with 2 missing per minute)\n",
           (double)hdr->sample_count / fs - 2.0 * ((double)hdr->sample_count / fs / 60.0));
    
    iqr_close(reader);
    return 0;
}
