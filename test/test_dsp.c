/**
 * @file test_dsp.c
 * @brief Unit tests for WWV signal processing math
 * 
 * Tests the biquad filter, DC blocker, and envelope detection
 * with synthetic signals to verify correct operation.
 * 
 * Build: gcc -o test_dsp test_dsp.c -lm
 * Run:   ./test_dsp
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdbool.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SAMPLE_RATE 48000.0f
#define TEST_DURATION_SEC 2.0f

/*============================================================================
 * Biquad Bandpass Filter (copied from wwv_scan.c)
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
 * DC Blocking Filter (copied from wwv_scan.c)
 *============================================================================*/

typedef struct {
    float prev_in;
    float prev_out;
    float alpha;
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
    float y = x - dc->prev_in + dc->alpha * dc->prev_out;
    dc->prev_in = x;
    dc->prev_out = y;
    return y;
}

/*============================================================================
 * Test Utilities
 *============================================================================*/

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (cond) { \
        printf("  PASS: %s\n", msg); \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s\n", msg); \
        tests_failed++; \
    } \
} while(0)

#define TEST_ASSERT_NEAR(val, expected, tolerance, msg) do { \
    float _v = (val), _e = (expected), _t = (tolerance); \
    if (fabsf(_v - _e) <= _t) { \
        printf("  PASS: %s (got %.4f, expected %.4f)\n", msg, _v, _e); \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s (got %.4f, expected %.4f, diff %.4f > %.4f)\n", \
               msg, _v, _e, fabsf(_v - _e), _t); \
        tests_failed++; \
    } \
} while(0)

/*============================================================================
 * Test: Biquad Bandpass Filter
 *============================================================================*/

void test_biquad_bandpass(void) {
    printf("\n=== Test: Biquad Bandpass Filter ===\n");
    
    biquad_t bp;
    biquad_init_bp(&bp, SAMPLE_RATE, 1000.0f, 5.0f);  /* 1000 Hz, Q=5 */
    
    /* Test 1: 1000 Hz sine should pass through with gain near 1 */
    printf("\nTest 1: 1000 Hz passband response\n");
    biquad_reset(&bp);
    
    float energy_in = 0, energy_out = 0;
    int samples = (int)(SAMPLE_RATE * 0.5f);  /* 0.5 seconds */
    
    /* Let filter settle first */
    for (int i = 0; i < 1000; i++) {
        float x = sinf(2.0f * M_PI * 1000.0f * i / SAMPLE_RATE);
        biquad_process(&bp, x);
    }
    
    /* Measure steady-state response */
    for (int i = 0; i < samples; i++) {
        float x = sinf(2.0f * M_PI * 1000.0f * i / SAMPLE_RATE);
        float y = biquad_process(&bp, x);
        energy_in += x * x;
        energy_out += y * y;
    }
    
    float gain_1000hz = sqrtf(energy_out / energy_in);
    TEST_ASSERT_NEAR(gain_1000hz, 1.0f, 0.1f, "1000 Hz gain near unity");
    
    /* Test 2: 100 Hz sine should be attenuated */
    printf("\nTest 2: 100 Hz stopband response\n");
    biquad_reset(&bp);
    
    energy_in = 0; energy_out = 0;
    for (int i = 0; i < 1000; i++) {
        float x = sinf(2.0f * M_PI * 100.0f * i / SAMPLE_RATE);
        biquad_process(&bp, x);
    }
    for (int i = 0; i < samples; i++) {
        float x = sinf(2.0f * M_PI * 100.0f * i / SAMPLE_RATE);
        float y = biquad_process(&bp, x);
        energy_in += x * x;
        energy_out += y * y;
    }
    
    float gain_100hz = sqrtf(energy_out / energy_in);
    TEST_ASSERT(gain_100hz < 0.2f, "100 Hz attenuated (gain < 0.2)");
    printf("    100 Hz gain: %.4f\n", gain_100hz);
    
    /* Test 3: 5000 Hz sine should be attenuated */
    printf("\nTest 3: 5000 Hz stopband response\n");
    biquad_reset(&bp);
    
    energy_in = 0; energy_out = 0;
    for (int i = 0; i < 1000; i++) {
        float x = sinf(2.0f * M_PI * 5000.0f * i / SAMPLE_RATE);
        biquad_process(&bp, x);
    }
    for (int i = 0; i < samples; i++) {
        float x = sinf(2.0f * M_PI * 5000.0f * i / SAMPLE_RATE);
        float y = biquad_process(&bp, x);
        energy_in += x * x;
        energy_out += y * y;
    }
    
    float gain_5000hz = sqrtf(energy_out / energy_in);
    TEST_ASSERT(gain_5000hz < 0.2f, "5000 Hz attenuated (gain < 0.2)");
    printf("    5000 Hz gain: %.4f\n", gain_5000hz);
}

/*============================================================================
 * Test: DC Blocking Filter
 *============================================================================*/

void test_dc_blocker(void) {
    printf("\n=== Test: DC Blocking Filter ===\n");
    
    dc_block_t dc;
    dc_block_init(&dc, 0.995f);
    
    /* Test 1: DC input should be removed */
    printf("\nTest 1: DC removal\n");
    dc_block_reset(&dc);
    
    float dc_level = 1.0f;
    float output_sum = 0;
    int samples = (int)(SAMPLE_RATE * 1.0f);
    
    /* Let filter settle */
    for (int i = 0; i < samples; i++) {
        dc_block_process(&dc, dc_level);
    }
    
    /* Measure output */
    for (int i = 0; i < samples; i++) {
        float y = dc_block_process(&dc, dc_level);
        output_sum += fabsf(y);
    }
    
    float avg_output = output_sum / samples;
    TEST_ASSERT(avg_output < 0.01f, "DC component removed (output < 0.01)");
    printf("    Avg output: %.6f\n", avg_output);
    
    /* Test 2: 1000 Hz AC should pass through */
    printf("\nTest 2: 1000 Hz AC passthrough\n");
    dc_block_reset(&dc);
    
    float energy_in = 0, energy_out = 0;
    samples = (int)(SAMPLE_RATE * 0.5f);
    
    for (int i = 0; i < 1000; i++) {
        float x = sinf(2.0f * M_PI * 1000.0f * i / SAMPLE_RATE);
        dc_block_process(&dc, x);
    }
    
    for (int i = 0; i < samples; i++) {
        float x = sinf(2.0f * M_PI * 1000.0f * i / SAMPLE_RATE);
        float y = dc_block_process(&dc, x);
        energy_in += x * x;
        energy_out += y * y;
    }
    
    float gain = sqrtf(energy_out / energy_in);
    TEST_ASSERT_NEAR(gain, 1.0f, 0.05f, "1000 Hz passes through");
}

/*============================================================================
 * Test: Full WWV-like Signal Processing Chain
 *============================================================================*/

void test_wwv_signal_chain(void) {
    printf("\n=== Test: WWV Signal Processing Chain ===\n");
    
    dc_block_t dc;
    biquad_t bp;
    
    dc_block_init(&dc, 0.995f);
    biquad_init_bp(&bp, SAMPLE_RATE, 1000.0f, 5.0f);
    
    /* 
     * Simulate WWV signal:
     * - Carrier (DC in envelope domain)
     * - 1000 Hz tick modulation (5ms every second)
     * 
     * In I/Q domain: carrier is constant magnitude
     * With AM modulation: magnitude = carrier * (1 + m * sin(2*pi*1000*t))
     * After envelope detection: we get the modulation
     */
    
    printf("\nTest: Tick vs Noise energy measurement\n");
    
    float carrier = 1.0f;
    float mod_depth = 0.5f;  /* 50% modulation */
    
    /* Simulate 1 second of signal */
    int samples_per_sec = (int)SAMPLE_RATE;
    int tick_end = (int)(0.005f * SAMPLE_RATE);  /* 5ms tick */
    
    double tick_energy = 0;
    int tick_count = 0;
    double noise_energy = 0;
    int noise_count = 0;
    
    /* Window boundaries (matching wwv_scan) */
    int tick_start_sample = 0;
    int tick_end_sample = (int)(0.050f * SAMPLE_RATE);   /* 0-50ms */
    int noise_start_sample = (int)(0.200f * SAMPLE_RATE); /* 200-800ms */
    int noise_end_sample = (int)(0.800f * SAMPLE_RATE);
    
    /* Reset filters */
    dc_block_reset(&dc);
    biquad_reset(&bp);
    
    /* Let filters settle with carrier-only signal */
    for (int i = 0; i < 10000; i++) {
        float envelope = carrier;  /* No modulation during settling */
        float ac = dc_block_process(&dc, envelope);
        biquad_process(&bp, ac);
    }
    
    /* Process one second */
    for (int i = 0; i < samples_per_sec; i++) {
        float envelope;
        
        /* During tick (first 5ms): carrier + 1000 Hz modulation */
        if (i < tick_end) {
            float t = (float)i / SAMPLE_RATE;
            envelope = carrier * (1.0f + mod_depth * sinf(2.0f * M_PI * 1000.0f * t));
        } else {
            /* Between ticks: just carrier */
            envelope = carrier;
        }
        
        /* Signal chain */
        float ac = dc_block_process(&dc, envelope);
        float filtered = biquad_process(&bp, ac);
        float energy = filtered * filtered;
        
        /* Accumulate in windows */
        if (i >= tick_start_sample && i < tick_end_sample) {
            tick_energy += energy;
            tick_count++;
        } else if (i >= noise_start_sample && i < noise_end_sample) {
            noise_energy += energy;
            noise_count++;
        }
    }
    
    double tick_avg = tick_energy / tick_count;
    double noise_avg = noise_energy / noise_count;
    double snr_db = 10.0 * log10(tick_avg / noise_avg);
    
    printf("    Tick energy avg:  %.6e\n", tick_avg);
    printf("    Noise energy avg: %.6e\n", noise_avg);
    printf("    SNR: %.1f dB\n", snr_db);
    
    TEST_ASSERT(snr_db > 10.0, "SNR > 10 dB with 50% modulation");
    TEST_ASSERT(tick_avg > noise_avg * 10, "Tick energy >> noise energy");
}

/*============================================================================
 * Test: What happens with DC-only signal (no modulation)
 *============================================================================*/

void test_dc_only_signal(void) {
    printf("\n=== Test: DC-only Signal (no modulation) ===\n");
    
    dc_block_t dc;
    biquad_t bp;
    
    dc_block_init(&dc, 0.995f);
    biquad_init_bp(&bp, SAMPLE_RATE, 1000.0f, 5.0f);
    
    float carrier = 1.0f;
    
    int samples_per_sec = (int)SAMPLE_RATE;
    int tick_start_sample = 0;
    int tick_end_sample = (int)(0.050f * SAMPLE_RATE);
    int noise_start_sample = (int)(0.200f * SAMPLE_RATE);
    int noise_end_sample = (int)(0.800f * SAMPLE_RATE);
    
    double tick_energy = 0;
    int tick_count = 0;
    double noise_energy = 0;
    int noise_count = 0;
    
    /* Reset and settle */
    dc_block_reset(&dc);
    biquad_reset(&bp);
    for (int i = 0; i < 10000; i++) {
        float ac = dc_block_process(&dc, carrier);
        biquad_process(&bp, ac);
    }
    
    /* Process - constant carrier, no modulation */
    for (int i = 0; i < samples_per_sec; i++) {
        float ac = dc_block_process(&dc, carrier);
        float filtered = biquad_process(&bp, ac);
        float energy = filtered * filtered;
        
        if (i >= tick_start_sample && i < tick_end_sample) {
            tick_energy += energy;
            tick_count++;
        } else if (i >= noise_start_sample && i < noise_end_sample) {
            noise_energy += energy;
            noise_count++;
        }
    }
    
    double tick_avg = tick_energy / tick_count;
    double noise_avg = noise_energy / noise_count;
    double snr_db = (noise_avg > 1e-20) ? 10.0 * log10(tick_avg / noise_avg) : 0;
    
    printf("    Tick energy avg:  %.6e\n", tick_avg);
    printf("    Noise energy avg: %.6e\n", noise_avg);
    printf("    SNR: %.1f dB\n", snr_db);
    
    /* With DC-only input, both should be near zero after filtering */
    TEST_ASSERT(tick_avg < 1e-6, "Tick energy near zero with no modulation");
    TEST_ASSERT(noise_avg < 1e-6, "Noise energy near zero with no modulation");
    TEST_ASSERT(fabsf(snr_db) < 3.0, "SNR near 0 dB (both windows similar)");
}

/*============================================================================
 * Main
 *============================================================================*/

int main(void) {
    printf("===========================================\n");
    printf("Phoenix SDR DSP Unit Tests\n");
    printf("===========================================\n");
    
    test_biquad_bandpass();
    test_dc_blocker();
    test_wwv_signal_chain();
    test_dc_only_signal();
    
    printf("\n===========================================\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("===========================================\n");
    
    return (tests_failed > 0) ? 1 : 0;
}
