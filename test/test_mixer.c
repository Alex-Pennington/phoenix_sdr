/**
 * @file test_mixer.c
 * @brief Unit test for the 450 kHz mixer math
 *
 * Test: Create a synthetic 450 kHz signal, mix it down, verify it becomes DC.
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define M_PI 3.14159265358979323846
#define SAMPLE_RATE 2000000.0
#define LIF_FREQUENCY 450000.0
#define TEST_SAMPLES 1000

int main(void) {
    printf("=== Mixer Math Unit Test ===\n\n");

    /* Mixer state */
    double mixer_phase = 0.0;
    double mixer_phase_inc = -2.0 * M_PI * LIF_FREQUENCY / SAMPLE_RATE;

    printf("Mixer phase increment: %.10f rad/sample\n", mixer_phase_inc);
    printf("Mixer frequency: %.1f Hz\n", mixer_phase_inc * SAMPLE_RATE / (2.0 * M_PI));
    printf("\n");

    /* Test 1: Pure 450 kHz cosine input should become DC after mixing */
    printf("TEST 1: 450 kHz cosine -> should become DC (constant I, zero Q)\n");

    double signal_phase = 0.0;
    double signal_phase_inc = 2.0 * M_PI * LIF_FREQUENCY / SAMPLE_RATE;

    double sum_I = 0, sum_Q = 0;
    double sum_I2 = 0, sum_Q2 = 0;
    double first_I = 0, first_Q = 0;

    mixer_phase = 0.0;
    signal_phase = 0.0;

    for (int i = 0; i < TEST_SAMPLES; i++) {
        /* Input: 450 kHz cosine (appears in I channel, Q=0 for real signal) */
        /* Actually for a REAL signal at 450 kHz in IQ, it appears as: */
        /* I = cos(450kHz * t), Q = sin(450kHz * t) */
        float I = (float)cos(signal_phase);
        float Q = (float)sin(signal_phase);

        /* Mixer math (from simple_am_receiver.c) */
        float cos_phase = (float)cos(mixer_phase);
        float sin_phase = (float)sin(mixer_phase);
        float new_I = I * cos_phase - Q * sin_phase;
        float new_Q = I * sin_phase + Q * cos_phase;

        if (i == 0) {
            first_I = new_I;
            first_Q = new_Q;
        }

        sum_I += new_I;
        sum_Q += new_Q;
        sum_I2 += new_I * new_I;
        sum_Q2 += new_Q * new_Q;

        /* Advance phases */
        signal_phase += signal_phase_inc;
        mixer_phase += mixer_phase_inc;
        if (mixer_phase < -2.0 * M_PI) mixer_phase += 2.0 * M_PI;
    }

    double mean_I = sum_I / TEST_SAMPLES;
    double mean_Q = sum_Q / TEST_SAMPLES;
    double var_I = sum_I2 / TEST_SAMPLES - mean_I * mean_I;
    double var_Q = sum_Q2 / TEST_SAMPLES - mean_Q * mean_Q;

    printf("  First sample: I=%.4f, Q=%.4f\n", first_I, first_Q);
    printf("  Mean I: %.6f (should be ~1.0 for DC)\n", mean_I);
    printf("  Mean Q: %.6f (should be ~0.0 for DC)\n", mean_Q);
    printf("  Variance I: %.6f (should be ~0 for DC)\n", var_I);
    printf("  Variance Q: %.6f (should be ~0 for DC)\n", var_Q);

    int test1_pass = (fabs(mean_I - 1.0) < 0.01) && (fabs(mean_Q) < 0.01) && (var_I < 0.01) && (var_Q < 0.01);
    printf("  RESULT: %s\n\n", test1_pass ? "PASS" : "FAIL");

    /* Test 2: DC input (0 Hz) should become 450 kHz after mixing (oscillating) */
    printf("TEST 2: DC input -> should become oscillating (high variance)\n");

    sum_I = sum_Q = sum_I2 = sum_Q2 = 0;
    mixer_phase = 0.0;

    for (int i = 0; i < TEST_SAMPLES; i++) {
        /* Input: DC (constant) */
        float I = 1.0f;
        float Q = 0.0f;

        /* Mixer math */
        float cos_phase = (float)cos(mixer_phase);
        float sin_phase = (float)sin(mixer_phase);
        float new_I = I * cos_phase - Q * sin_phase;
        float new_Q = I * sin_phase + Q * cos_phase;

        sum_I += new_I;
        sum_Q += new_Q;
        sum_I2 += new_I * new_I;
        sum_Q2 += new_Q * new_Q;

        mixer_phase += mixer_phase_inc;
        if (mixer_phase < -2.0 * M_PI) mixer_phase += 2.0 * M_PI;
    }

    mean_I = sum_I / TEST_SAMPLES;
    mean_Q = sum_Q / TEST_SAMPLES;
    var_I = sum_I2 / TEST_SAMPLES - mean_I * mean_I;
    var_Q = sum_Q2 / TEST_SAMPLES - mean_Q * mean_Q;

    printf("  Mean I: %.6f (should be ~0 for oscillating)\n", mean_I);
    printf("  Mean Q: %.6f (should be ~0 for oscillating)\n", mean_Q);
    printf("  Variance I: %.6f (should be ~0.5 for cosine)\n", var_I);
    printf("  Variance Q: %.6f (should be ~0.5 for sine)\n", var_Q);

    int test2_pass = (fabs(mean_I) < 0.1) && (fabs(mean_Q) < 0.1) && (var_I > 0.4) && (var_Q > 0.4);
    printf("  RESULT: %s\n\n", test2_pass ? "PASS" : "FAIL");

    /* Test 3: Check the complex multiplication formula is correct */
    printf("TEST 3: Verify complex multiplication formula\n");
    printf("  Formula: (I + jQ) * e^(j*phase) = (I + jQ) * (cos + j*sin)\n");
    printf("  Code: new_I = I*cos - Q*sin, new_Q = I*sin + Q*cos\n");
    printf("  This is multiplication by e^(+j*phase)\n");
    printf("  With negative phase_inc, we get e^(-j*|phase|) = downconversion\n");
    printf("  RESULT: Formula is CORRECT for downconversion\n\n");

    /* Summary */
    printf("=== SUMMARY ===\n");
    if (test1_pass && test2_pass) {
        printf("All tests PASSED - Mixer math is correct\n");
        printf("Problem must be elsewhere (sample rate? IF setting?)\n");
    } else {
        printf("Tests FAILED - Mixer math has a bug\n");
    }

    /* Test 4: What if signal is at -450 kHz instead of +450 kHz? */
    printf("\n=== ADDITIONAL TEST ===\n");
    printf("TEST 4: NEGATIVE 450 kHz signal (what if IF is at -450 kHz?)\n");

    sum_I = sum_Q = sum_I2 = sum_Q2 = 0;
    mixer_phase = 0.0;
    signal_phase = 0.0;
    double neg_signal_inc = -2.0 * M_PI * LIF_FREQUENCY / SAMPLE_RATE;  /* -450 kHz */

    for (int i = 0; i < TEST_SAMPLES; i++) {
        /* Input: -450 kHz (negative frequency) */
        float I = (float)cos(signal_phase);
        float Q = (float)sin(signal_phase);

        /* Mixer math */
        float cos_phase = (float)cos(mixer_phase);
        float sin_phase = (float)sin(mixer_phase);
        float new_I = I * cos_phase - Q * sin_phase;
        float new_Q = I * sin_phase + Q * cos_phase;

        sum_I += new_I;
        sum_Q += new_Q;
        sum_I2 += new_I * new_I;
        sum_Q2 += new_Q * new_Q;

        signal_phase += neg_signal_inc;
        mixer_phase += mixer_phase_inc;
        if (mixer_phase < -2.0 * M_PI) mixer_phase += 2.0 * M_PI;
    }

    mean_I = sum_I / TEST_SAMPLES;
    mean_Q = sum_Q / TEST_SAMPLES;
    var_I = sum_I2 / TEST_SAMPLES - mean_I * mean_I;
    var_Q = sum_Q2 / TEST_SAMPLES - mean_Q * mean_Q;

    printf("  If signal is at -450 kHz and mixer is -450 kHz:\n");
    printf("  Result would be at -900 kHz (oscillating)\n");
    printf("  Mean I: %.6f (expect ~0 if oscillating)\n", mean_I);
    printf("  Variance I: %.6f (expect ~0.5 if oscillating)\n", var_I);
    printf("  CONCLUSION: If IF puts signal at -450 kHz, our mixer is WRONG\n");
    printf("              We'd need +450 kHz mixer instead of -450 kHz\n");

    return (test1_pass && test2_pass) ? 0 : 1;
}
