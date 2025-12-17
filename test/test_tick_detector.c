/**
 * @file test_tick_detector.c
 * @brief Unit tests for tick_detector module
 *
 * Tests the WWV 1-second tick pulse detection algorithm:
 * - Create/destroy lifecycle
 * - Sample processing
 * - Detection callbacks
 * - Edge cases
 */

#include "test_framework.h"
#include "../tools/tick_detector.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*============================================================================
 * Test Helpers
 *============================================================================*/

/* Callback tracking */
static int g_tick_callback_count = 0;
static tick_event_t g_last_tick_event;

static void test_tick_callback(const tick_event_t *event, void *user_data) {
    (void)user_data;
    g_tick_callback_count++;
    if (event) {
        g_last_tick_event = *event;
    }
}

static int g_marker_callback_count = 0;
static tick_marker_event_t g_last_marker_event;

static void test_marker_callback(const tick_marker_event_t *event, void *user_data) {
    (void)user_data;
    g_marker_callback_count++;
    if (event) {
        g_last_marker_event = *event;
    }
}

static void reset_callback_state(void) {
    g_tick_callback_count = 0;
    g_marker_callback_count = 0;
    memset(&g_last_tick_event, 0, sizeof(g_last_tick_event));
    memset(&g_last_marker_event, 0, sizeof(g_last_marker_event));
}

/* Generate synthetic I/Q tick pulse (5ms, 1000Hz tone burst) */
static void generate_tick_iq(tick_detector_t *td, float freq, int duration_samples) {
    for (int i = 0; i < duration_samples; i++) {
        float t = (float)i / TICK_SAMPLE_RATE;
        float phase = 2.0f * M_PI * freq * t;
        float i_sample = 0.8f * cosf(phase);
        float q_sample = 0.8f * sinf(phase);
        tick_detector_process_sample(td, i_sample, q_sample);
    }
}

/* Feed noise samples */
static void feed_noise(tick_detector_t *td, int num_samples, float amplitude) {
    for (int i = 0; i < num_samples; i++) {
        float i_sample = amplitude * ((float)rand() / RAND_MAX * 2.0f - 1.0f);
        float q_sample = amplitude * ((float)rand() / RAND_MAX * 2.0f - 1.0f);
        tick_detector_process_sample(td, i_sample, q_sample);
    }
}

/* Feed silence */
static void feed_silence(tick_detector_t *td, int num_samples) {
    for (int i = 0; i < num_samples; i++) {
        tick_detector_process_sample(td, 0.0f, 0.0f);
    }
}

/*============================================================================
 * Lifecycle Tests
 *============================================================================*/

TEST(tick_create_destroy) {
    tick_detector_t *det = tick_detector_create(NULL);  /* No CSV logging */
    ASSERT_NOT_NULL(det, "tick_detector_create should return non-NULL");

    tick_detector_destroy(det);
    PASS();
}

TEST(tick_create_with_csv) {
    /* Create with CSV path - may or may not succeed depending on filesystem */
    tick_detector_t *det = tick_detector_create("test_ticks.csv");
    if (det != NULL) {
        tick_detector_destroy(det);
        /* Clean up test file */
        remove("test_ticks.csv");
    }
    /* Either way, shouldn't crash */
    PASS();
}

TEST(tick_destroy_null) {
    /* Should not crash on NULL */
    tick_detector_destroy(NULL);
    PASS();
}

TEST(tick_callback_registration) {
    tick_detector_t *det = tick_detector_create(NULL);
    ASSERT_NOT_NULL(det, "create should succeed");

    tick_detector_set_callback(det, test_tick_callback, NULL);
    tick_detector_set_marker_callback(det, test_marker_callback, NULL);

    tick_detector_destroy(det);
    PASS();
}

/*============================================================================
 * State Tests
 *============================================================================*/

TEST(tick_enable_disable) {
    tick_detector_t *det = tick_detector_create(NULL);
    ASSERT_NOT_NULL(det, "create should succeed");

    /* Should start enabled */
    ASSERT_TRUE(tick_detector_get_enabled(det), "should start enabled");

    /* Disable */
    tick_detector_set_enabled(det, false);
    ASSERT_FALSE(tick_detector_get_enabled(det), "should be disabled");

    /* Re-enable */
    tick_detector_set_enabled(det, true);
    ASSERT_TRUE(tick_detector_get_enabled(det), "should be re-enabled");

    tick_detector_destroy(det);
    PASS();
}

TEST(tick_flash_frames) {
    tick_detector_t *det = tick_detector_create(NULL);
    ASSERT_NOT_NULL(det, "create should succeed");

    /* Initially no flash */
    int flash = tick_detector_get_flash_frames(det);
    ASSERT_EQ(flash, 0, "should not be flashing initially");

    /* Decrement on zero should not underflow */
    tick_detector_decrement_flash(det);
    flash = tick_detector_get_flash_frames(det);
    ASSERT_EQ(flash, 0, "should still be 0 after decrement");

    tick_detector_destroy(det);
    PASS();
}

TEST(tick_get_stats) {
    tick_detector_t *det = tick_detector_create(NULL);
    ASSERT_NOT_NULL(det, "create should succeed");

    /* These should not crash and return valid initial values */
    float noise_floor = tick_detector_get_noise_floor(det);
    float threshold = tick_detector_get_threshold(det);
    float energy = tick_detector_get_current_energy(det);
    int count = tick_detector_get_tick_count(det);

    /* Noise floor and threshold should be positive or zero */
    ASSERT_TRUE(noise_floor >= 0.0f, "noise floor should be >= 0");
    ASSERT_TRUE(threshold >= 0.0f, "threshold should be >= 0");
    ASSERT_TRUE(energy >= 0.0f, "energy should be >= 0");
    ASSERT_EQ(count, 0, "initial tick count should be 0");

    tick_detector_destroy(det);
    PASS();
}

/*============================================================================
 * Processing Tests
 *============================================================================*/

TEST(tick_process_silence) {
    tick_detector_t *det = tick_detector_create(NULL);
    reset_callback_state();
    tick_detector_set_callback(det, test_tick_callback, NULL);

    /* Feed 1 second of silence - should not trigger */
    feed_silence(det, TICK_SAMPLE_RATE);

    ASSERT_EQ(g_tick_callback_count, 0, "silence should not trigger ticks");

    tick_detector_destroy(det);
    PASS();
}

TEST(tick_process_noise) {
    tick_detector_t *det = tick_detector_create(NULL);
    reset_callback_state();
    tick_detector_set_callback(det, test_tick_callback, NULL);

    /* Feed low-level noise - should not trigger many false positives */
    feed_noise(det, TICK_SAMPLE_RATE, 0.01f);

    /* With proper threshold, false positives should be minimal */
    ASSERT_LT(g_tick_callback_count, 5, "noise should not trigger many false positives");

    tick_detector_destroy(det);
    PASS();
}

TEST(tick_process_tone_burst) {
    tick_detector_t *det = tick_detector_create(NULL);
    reset_callback_state();
    tick_detector_set_callback(det, test_tick_callback, NULL);

    /* Generate a 5ms tone burst at 1000 Hz (WWV tick) */
    int tick_samples = (TICK_SAMPLE_RATE * 5) / 1000;  /* 5ms */

    /* Warm-up with some noise */
    feed_noise(det, TICK_SAMPLE_RATE / 2, 0.01f);

    /* Generate tone burst */
    generate_tick_iq(det, 1000.0f, tick_samples);

    /* More samples to let detector process */
    feed_noise(det, TICK_SAMPLE_RATE / 2, 0.01f);

    /* Should detect the tick */
    /* Note: detector has holdoff so may not trigger immediately */
    ASSERT_GT(g_tick_callback_count, 0, "should detect 1000 Hz tone burst");

    tick_detector_destroy(det);
    PASS();
}

TEST(tick_process_disabled) {
    tick_detector_t *det = tick_detector_create(NULL);
    reset_callback_state();
    tick_detector_set_callback(det, test_tick_callback, NULL);

    /* Disable detection */
    tick_detector_set_enabled(det, false);

    /* Generate a clear tick - should NOT trigger when disabled */
    int tick_samples = (TICK_SAMPLE_RATE * 5) / 1000;
    generate_tick_iq(det, 1000.0f, tick_samples);
    feed_noise(det, TICK_SAMPLE_RATE / 4, 0.01f);

    ASSERT_EQ(g_tick_callback_count, 0, "disabled detector should not trigger");

    tick_detector_destroy(det);
    PASS();
}

/*============================================================================
 * Metadata Logging Tests
 *============================================================================*/

TEST(tick_log_metadata) {
    tick_detector_t *det = tick_detector_create(NULL);
    ASSERT_NOT_NULL(det, "create should succeed");

    /* These should not crash even without CSV */
    tick_detector_log_metadata(det, 5000000, 2000000, 40, 3);
    tick_detector_log_display_gain(det, 10.0f);

    tick_detector_destroy(det);
    PASS();
}

TEST(tick_print_stats) {
    tick_detector_t *det = tick_detector_create(NULL);
    ASSERT_NOT_NULL(det, "create should succeed");

    /* Should not crash */
    tick_detector_print_stats(det);

    tick_detector_destroy(det);
    PASS();
}

/*============================================================================
 * Constants Tests
 *============================================================================*/

TEST(tick_frame_duration) {
    float duration = tick_detector_get_frame_duration_ms();
    
    /* Should be reasonable value (around 5ms for 256-pt FFT at 50kHz) */
    ASSERT_GT(duration, 0.0f, "frame duration should be positive");
    ASSERT_LT(duration, 100.0f, "frame duration should be reasonable");

    PASS();
}

/*============================================================================
 * Main
 *============================================================================*/

int main(void) {
    TEST_BEGIN("Tick Detector Unit Tests");

    TEST_SECTION("Lifecycle");
    RUN_TEST(tick_create_destroy);
    RUN_TEST(tick_create_with_csv);
    RUN_TEST(tick_destroy_null);
    RUN_TEST(tick_callback_registration);

    TEST_SECTION("State Management");
    RUN_TEST(tick_enable_disable);
    RUN_TEST(tick_flash_frames);
    RUN_TEST(tick_get_stats);

    TEST_SECTION("Processing");
    RUN_TEST(tick_process_silence);
    RUN_TEST(tick_process_noise);
    RUN_TEST(tick_process_tone_burst);
    RUN_TEST(tick_process_disabled);

    TEST_SECTION("Metadata Logging");
    RUN_TEST(tick_log_metadata);
    RUN_TEST(tick_print_stats);

    TEST_SECTION("Constants");
    RUN_TEST(tick_frame_duration);

    TEST_END();
    return TEST_EXIT_CODE();
}
