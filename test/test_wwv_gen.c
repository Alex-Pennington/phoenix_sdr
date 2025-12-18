/**
 * @file test_wwv_gen.c
 * @brief Unit tests for WWV signal generator modules
 */

#include "test_framework.h"
#include "../tools/oscillator.h"
#include "../tools/bcd_encoder.h"
#include "../tools/wwv_signal.h"
#include <math.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SAMPLE_RATE 2000000
#define EPSILON 0.01f

// Test oscillator phase continuity
TEST(oscillator_phase_continuity) {
    oscillator_t *osc = oscillator_create(SAMPLE_RATE, 1000.0f);
    ASSERT(osc != NULL, "Failed to create oscillator");

    float prev_i = 0.0f, prev_q = 0.0f;
    float i, q;

    // Generate 1000 samples and check for phase jumps
    oscillator_get_iq(osc, &prev_i, &prev_q);

    for (int n = 1; n < 1000; n++) {
        oscillator_get_iq(osc, &i, &q);

        // Calculate phase difference (should be small for continuous phase)
        float phase_prev = atan2f(prev_q, prev_i);
        float phase_curr = atan2f(q, i);
        float phase_diff = phase_curr - phase_prev;

        // Wrap phase difference to [-π, π]
        while (phase_diff > M_PI) phase_diff -= 2.0f * M_PI;
        while (phase_diff < -M_PI) phase_diff += 2.0f * M_PI;

        // Expected phase increment for 1000 Hz at 2 Msps
        float expected_increment = (2.0f * M_PI * 1000.0f) / SAMPLE_RATE;
        float error = fabsf(phase_diff - expected_increment);

        ASSERT(error < 0.01f, "Phase discontinuity detected");

        prev_i = i;
        prev_q = q;
    }

    oscillator_destroy(osc);
    PASS();
}

// Test oscillator amplitude control
TEST(oscillator_amplitude_control) {
    oscillator_t *osc = oscillator_create(SAMPLE_RATE, 1000.0f);
    ASSERT(osc != NULL, "Failed to create oscillator");

    oscillator_set_amplitude(osc, 0.5f);
    ASSERT(fabsf(oscillator_get_amplitude(osc) - 0.5f) < EPSILON, "Amplitude not set correctly");

    float i, q;
    oscillator_get_iq(osc, &i, &q);
    float magnitude = sqrtf(i * i + q * q);
    ASSERT(fabsf(magnitude - 0.5f) < EPSILON, "Output magnitude incorrect");

    oscillator_destroy(osc);
    PASS();
}

// Test BCD pulse widths
TEST(bcd_pulse_widths) {
    bcd_encoder_t *enc = bcd_encoder_create();
    ASSERT(enc != NULL, "Failed to create BCD encoder");

    // Set time: minute=5, hour=12, day=100, year=25
    // This creates a known pattern
    bcd_encoder_set_time(enc, 5, 12, 100, 25);

    // Second 5: minute units bit 0 = 1 (minute=5 has bit 0 set)
    bcd_symbol_t sym5 = bcd_encoder_get_symbol(enc, 5);
    ASSERT(sym5 == BCD_ONE, "Second 5 should be BCD_ONE");
    ASSERT(bcd_get_pulse_width_ms(sym5) == 500, "BCD_ONE should be 500ms");

    // Second 6: minute units bit 1 = 0 (minute=5 does not have bit 1 set)
    bcd_symbol_t sym6 = bcd_encoder_get_symbol(enc, 6);
    ASSERT(sym6 == BCD_ZERO, "Second 6 should be BCD_ZERO");
    ASSERT(bcd_get_pulse_width_ms(sym6) == 200, "BCD_ZERO should be 200ms");

    // Second 9: position marker P1
    bcd_symbol_t sym9 = bcd_encoder_get_symbol(enc, 9);
    ASSERT(sym9 == BCD_MARKER, "Second 9 should be BCD_MARKER");
    ASSERT(bcd_get_pulse_width_ms(sym9) == 800, "BCD_MARKER should be 800ms");

    bcd_encoder_destroy(enc);
    PASS();
}

// Test BCD modulation levels
TEST(bcd_modulation_levels) {
    bcd_encoder_t *enc = bcd_encoder_create();
    ASSERT(enc != NULL, "Failed to create BCD encoder");

    bcd_encoder_set_time(enc, 5, 12, 100, 25);

    // Second 5 (BCD_ONE): first 30ms low, then 500ms high, then low
    // Sample at 40ms (80000 samples) should be high
    int sample_40ms = 80000;
    float mod = bcd_encoder_get_modulation(enc, 5, sample_40ms);
    ASSERT(fabsf(mod - 0.18f) < 0.001f, "BCD high level should be 0.18");

    // Sample at 600ms (1200000 samples) should be low
    int sample_600ms = 1200000;
    mod = bcd_encoder_get_modulation(enc, 5, sample_600ms);
    ASSERT(fabsf(mod - 0.03f) < 0.001f, "BCD low level should be 0.03");

    // First 30ms should always be low
    int sample_10ms = 20000;
    mod = bcd_encoder_get_modulation(enc, 5, sample_10ms);
    ASSERT(fabsf(mod - 0.03f) < 0.001f, "First 30ms should be low");

    bcd_encoder_destroy(enc);
    PASS();
}

// Test tick timing
TEST(tick_timing) {
    wwv_signal_t *sig = wwv_signal_create(0, 0, 1, 25, WWV_STATION_WWV);
    ASSERT(sig != NULL, "Failed to create WWV signal");

    int16_t i, q;

    // Skip to second 1, sample 25000 (middle of tick pulse)
    // Tick is at samples 20000-30000
    for (int n = 0; n < 2000000 + 25000; n++) {
        wwv_signal_get_sample_int16(sig, &i, &q);
    }

    // Should have signal present (non-zero)
    ASSERT(abs(i) > 1000, "Tick signal should be present");

    // Skip to guard zone (sample 35000)
    for (int n = 0; n < 10000; n++) {
        wwv_signal_get_sample_int16(sig, &i, &q);
    }

    // Guard zone should have lower amplitude (BCD only)
    int guard_amplitude = abs(i);
    ASSERT(guard_amplitude < 10000, "Guard zone should have low signal");

    wwv_signal_destroy(sig);
    PASS();
}

// Test marker timing
TEST(marker_timing) {
    wwv_signal_t *sig = wwv_signal_create(0, 0, 1, 25, WWV_STATION_WWV);
    ASSERT(sig != NULL, "Failed to create WWV signal");

    int16_t i, q;

    // Sample at 500ms (middle of marker)
    for (int n = 0; n < 1000000; n++) {
        wwv_signal_get_sample_int16(sig, &i, &q);
    }

    // Should have strong signal (marker is 800ms from 10-810ms)
    ASSERT(abs(i) > 10000, "Marker signal should be strong");

    // Sample at 820ms (after marker, in guard zone)
    for (int n = 0; n < 640000; n++) {
        wwv_signal_get_sample_int16(sig, &i, &q);
    }

    // Guard zone should have lower amplitude
    ASSERT(abs(i) < 10000, "Post-marker guard should have low signal");

    wwv_signal_destroy(sig);
    PASS();
}

// Test tone schedule
TEST(tone_schedule) {
    // Test that minute 4 has 500 Hz tone, minute 1 has 600 Hz
    // This is a simplified test - full validation would require FFT

    wwv_signal_t *sig = wwv_signal_create(4, 0, 1, 25, WWV_STATION_WWV);
    ASSERT(sig != NULL, "Failed to create WWV signal");

    // Generate some samples
    int16_t i, q;
    for (int n = 0; n < 1000; n++) {
        wwv_signal_get_sample_int16(sig, &i, &q);
    }

    // Just verify signal is generated (tone validation requires spectral analysis)
    ASSERT(wwv_signal_get_sample_count(sig) == 1000, "Sample count mismatch");

    wwv_signal_destroy(sig);
    PASS();
}

// Test WWV vs WWVH station difference
TEST(station_type) {
    wwv_signal_t *wwv = wwv_signal_create(1, 0, 1, 25, WWV_STATION_WWV);
    wwv_signal_t *wwvh = wwv_signal_create(1, 0, 1, 25, WWV_STATION_WWVH);

    ASSERT(wwv != NULL, "Failed to create WWV signal");
    ASSERT(wwvh != NULL, "Failed to create WWVH signal");

    // Both should generate samples successfully
    int16_t i1, q1, i2, q2;
    wwv_signal_get_sample_int16(wwv, &i1, &q1);
    wwv_signal_get_sample_int16(wwvh, &i2, &q2);

    // Signals will differ due to different tick frequencies (1000 vs 1200 Hz)
    // This test just verifies both work

    wwv_signal_destroy(wwv);
    wwv_signal_destroy(wwvh);
    PASS();
}

int main(void) {
    printf("Running WWV Generator Tests...\n\n");

    RUN_TEST(oscillator_phase_continuity);
    RUN_TEST(oscillator_amplitude_control);
    RUN_TEST(bcd_pulse_widths);
    RUN_TEST(bcd_modulation_levels);
    RUN_TEST(tick_timing);
    RUN_TEST(marker_timing);
    RUN_TEST(tone_schedule);
    RUN_TEST(station_type);

    printf("\nAll tests passed!\n");
    return 0;
}
