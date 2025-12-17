/**
 * @file test_iq_recorder.c
 * @brief Unit tests for iq_recorder module
 *
 * Tests the I/Q sample recording and playback:
 * - Create/destroy lifecycle
 * - Recording operations
 * - Playback/reader operations
 * - File format validation
 * - Edge cases
 */

#include "test_framework.h"
#include "../include/iq_recorder.h"
#include <string.h>

/*============================================================================
 * Test Helpers
 *============================================================================*/

#define TEST_FILENAME "test_recording.iqr"

static void cleanup_test_file(void) {
    remove(TEST_FILENAME);
}

/*============================================================================
 * Error String Tests
 *============================================================================*/

TEST(iqr_strerror_codes) {
    /* All error codes should return non-NULL strings */
    ASSERT_NOT_NULL(iqr_strerror(IQR_OK), "OK should have string");
    ASSERT_NOT_NULL(iqr_strerror(IQR_ERR_INVALID_ARG), "INVALID_ARG should have string");
    ASSERT_NOT_NULL(iqr_strerror(IQR_ERR_FILE_OPEN), "FILE_OPEN should have string");
    ASSERT_NOT_NULL(iqr_strerror(IQR_ERR_FILE_WRITE), "FILE_WRITE should have string");
    ASSERT_NOT_NULL(iqr_strerror(IQR_ERR_FILE_READ), "FILE_READ should have string");
    ASSERT_NOT_NULL(iqr_strerror(IQR_ERR_FILE_SEEK), "FILE_SEEK should have string");
    ASSERT_NOT_NULL(iqr_strerror(IQR_ERR_NOT_RECORDING), "NOT_RECORDING should have string");
    ASSERT_NOT_NULL(iqr_strerror(IQR_ERR_ALREADY_RECORDING), "ALREADY_RECORDING should have string");
    ASSERT_NOT_NULL(iqr_strerror(IQR_ERR_INVALID_FORMAT), "INVALID_FORMAT should have string");
    ASSERT_NOT_NULL(iqr_strerror(IQR_ERR_VERSION_MISMATCH), "VERSION_MISMATCH should have string");
    ASSERT_NOT_NULL(iqr_strerror(IQR_ERR_ALLOC), "ALLOC should have string");

    /* Unknown code should still return something */
    ASSERT_NOT_NULL(iqr_strerror((iqr_error_t)999), "unknown code should have string");

    PASS();
}

/*============================================================================
 * Recorder Lifecycle Tests
 *============================================================================*/

TEST(iqr_create_destroy) {
    iqr_recorder_t *rec = NULL;
    iqr_error_t err = iqr_create(&rec, 0);

    ASSERT_EQ(err, IQR_OK, "create should succeed");
    ASSERT_NOT_NULL(rec, "recorder should be non-NULL");

    iqr_destroy(rec);
    PASS();
}

TEST(iqr_create_custom_buffer) {
    iqr_recorder_t *rec = NULL;
    iqr_error_t err = iqr_create(&rec, 4096);

    ASSERT_EQ(err, IQR_OK, "create with custom buffer should succeed");
    ASSERT_NOT_NULL(rec, "recorder should be non-NULL");

    iqr_destroy(rec);
    PASS();
}

TEST(iqr_create_null_ptr) {
    iqr_error_t err = iqr_create(NULL, 0);
    ASSERT_EQ(err, IQR_ERR_INVALID_ARG, "NULL pointer should return INVALID_ARG");
    PASS();
}

TEST(iqr_destroy_null) {
    /* Should not crash */
    iqr_destroy(NULL);
    PASS();
}

/*============================================================================
 * Recording Tests
 *============================================================================*/

TEST(iqr_start_stop) {
    cleanup_test_file();

    iqr_recorder_t *rec = NULL;
    iqr_create(&rec, 0);

    ASSERT_FALSE(iqr_is_recording(rec), "should not be recording initially");

    iqr_error_t err = iqr_start(rec, TEST_FILENAME, 2000000.0, 5000000.0, 600, 40, 3);
    ASSERT_EQ(err, IQR_OK, "start should succeed");
    ASSERT_TRUE(iqr_is_recording(rec), "should be recording after start");

    err = iqr_stop(rec);
    ASSERT_EQ(err, IQR_OK, "stop should succeed");
    ASSERT_FALSE(iqr_is_recording(rec), "should not be recording after stop");

    iqr_destroy(rec);
    cleanup_test_file();
    PASS();
}

TEST(iqr_double_start) {
    cleanup_test_file();

    iqr_recorder_t *rec = NULL;
    iqr_create(&rec, 0);

    iqr_start(rec, TEST_FILENAME, 2000000.0, 5000000.0, 600, 40, 3);

    /* Second start should fail */
    iqr_error_t err = iqr_start(rec, "other.iqr", 2000000.0, 5000000.0, 600, 40, 3);
    ASSERT_EQ(err, IQR_ERR_ALREADY_RECORDING, "double start should fail");

    iqr_stop(rec);
    iqr_destroy(rec);
    cleanup_test_file();
    PASS();
}

TEST(iqr_stop_not_recording) {
    iqr_recorder_t *rec = NULL;
    iqr_create(&rec, 0);

    /* Stop without start should fail */
    iqr_error_t err = iqr_stop(rec);
    ASSERT_EQ(err, IQR_ERR_NOT_RECORDING, "stop without start should fail");

    iqr_destroy(rec);
    PASS();
}

TEST(iqr_write_samples) {
    cleanup_test_file();

    iqr_recorder_t *rec = NULL;
    iqr_create(&rec, 0);
    iqr_start(rec, TEST_FILENAME, 2000000.0, 5000000.0, 600, 40, 3);

    /* Write some samples */
    int16_t xi[1024], xq[1024];
    for (int i = 0; i < 1024; i++) {
        xi[i] = (int16_t)(i * 10);
        xq[i] = (int16_t)(i * -10);
    }

    iqr_error_t err = iqr_write(rec, xi, xq, 1024);
    ASSERT_EQ(err, IQR_OK, "write should succeed");

    ASSERT_EQ(iqr_get_sample_count(rec), 1024, "sample count should be 1024");

    /* Write more */
    err = iqr_write(rec, xi, xq, 1024);
    ASSERT_EQ(err, IQR_OK, "second write should succeed");
    ASSERT_EQ(iqr_get_sample_count(rec), 2048, "sample count should be 2048");

    iqr_stop(rec);
    iqr_destroy(rec);
    cleanup_test_file();
    PASS();
}

TEST(iqr_write_not_recording) {
    iqr_recorder_t *rec = NULL;
    iqr_create(&rec, 0);

    int16_t xi[16] = {0}, xq[16] = {0};
    iqr_error_t err = iqr_write(rec, xi, xq, 16);
    ASSERT_EQ(err, IQR_ERR_NOT_RECORDING, "write without recording should fail");

    iqr_destroy(rec);
    PASS();
}

TEST(iqr_duration) {
    cleanup_test_file();

    iqr_recorder_t *rec = NULL;
    iqr_create(&rec, 0);
    iqr_start(rec, TEST_FILENAME, 1000.0, 5000000.0, 600, 40, 3);  /* 1000 Hz sample rate */

    /* Write 500 samples at 1000 Hz = 0.5 seconds */
    int16_t xi[500] = {0}, xq[500] = {0};
    iqr_write(rec, xi, xq, 500);

    double duration = iqr_get_duration(rec);
    ASSERT_FLOAT_EQ(duration, 0.5, 0.001, "duration should be 0.5 seconds");

    iqr_stop(rec);
    iqr_destroy(rec);
    cleanup_test_file();
    PASS();
}

/*============================================================================
 * Reader Tests
 *============================================================================*/

TEST(iqr_reader_open_close) {
    cleanup_test_file();

    /* Create a recording first */
    iqr_recorder_t *rec = NULL;
    iqr_create(&rec, 0);
    iqr_start(rec, TEST_FILENAME, 2000000.0, 5000000.0, 600, 40, 3);
    int16_t xi[100] = {0}, xq[100] = {0};
    iqr_write(rec, xi, xq, 100);
    iqr_stop(rec);
    iqr_destroy(rec);

    /* Now open as reader */
    iqr_reader_t *reader = NULL;
    iqr_error_t err = iqr_open(&reader, TEST_FILENAME);
    ASSERT_EQ(err, IQR_OK, "open should succeed");
    ASSERT_NOT_NULL(reader, "reader should be non-NULL");

    iqr_close(reader);
    cleanup_test_file();
    PASS();
}

TEST(iqr_reader_open_nonexistent) {
    iqr_reader_t *reader = NULL;
    iqr_error_t err = iqr_open(&reader, "nonexistent_file.iqr");
    ASSERT_EQ(err, IQR_ERR_FILE_OPEN, "opening nonexistent file should fail");
    ASSERT_NULL(reader, "reader should be NULL on failure");
    PASS();
}

TEST(iqr_reader_close_null) {
    /* Should not crash */
    iqr_close(NULL);
    PASS();
}

TEST(iqr_reader_header) {
    cleanup_test_file();

    /* Create recording with known parameters */
    iqr_recorder_t *rec = NULL;
    iqr_create(&rec, 0);
    iqr_start(rec, TEST_FILENAME, 2000000.0, 5000000.0, 600, 40, 3);
    int16_t xi[256] = {0}, xq[256] = {0};
    iqr_write(rec, xi, xq, 256);
    iqr_stop(rec);
    iqr_destroy(rec);

    /* Read back and verify header */
    iqr_reader_t *reader = NULL;
    iqr_open(&reader, TEST_FILENAME);

    const iqr_header_t *hdr = iqr_get_header(reader);
    ASSERT_NOT_NULL(hdr, "header should be non-NULL");
    ASSERT_EQ(memcmp(hdr->magic, IQR_MAGIC, 4), 0, "magic should match");
    ASSERT_EQ(hdr->version, IQR_VERSION, "version should match");
    ASSERT_FLOAT_EQ(hdr->sample_rate_hz, 2000000.0, 1.0, "sample rate should match");
    ASSERT_FLOAT_EQ(hdr->center_freq_hz, 5000000.0, 1.0, "center freq should match");
    ASSERT_EQ(hdr->bandwidth_khz, 600, "bandwidth should match");
    ASSERT_EQ(hdr->gain_reduction, 40, "gain reduction should match");
    ASSERT_EQ(hdr->lna_state, 3, "LNA state should match");
    ASSERT_EQ(hdr->sample_count, 256, "sample count should match");

    iqr_close(reader);
    cleanup_test_file();
    PASS();
}

TEST(iqr_reader_read_samples) {
    cleanup_test_file();

    /* Create recording with known data */
    iqr_recorder_t *rec = NULL;
    iqr_create(&rec, 0);
    iqr_start(rec, TEST_FILENAME, 2000000.0, 5000000.0, 600, 40, 3);

    int16_t xi_write[100], xq_write[100];
    for (int i = 0; i < 100; i++) {
        xi_write[i] = (int16_t)(i * 100);
        xq_write[i] = (int16_t)(i * -100);
    }
    iqr_write(rec, xi_write, xq_write, 100);
    iqr_stop(rec);
    iqr_destroy(rec);

    /* Read back and verify data */
    iqr_reader_t *reader = NULL;
    iqr_open(&reader, TEST_FILENAME);

    int16_t xi_read[100], xq_read[100];
    uint32_t num_read = 0;
    iqr_error_t err = iqr_read(reader, xi_read, xq_read, 100, &num_read);
    ASSERT_EQ(err, IQR_OK, "read should succeed");
    ASSERT_EQ(num_read, 100, "should read 100 samples");

    /* Verify data matches */
    for (int i = 0; i < 100; i++) {
        ASSERT_EQ(xi_read[i], xi_write[i], "I sample should match");
        ASSERT_EQ(xq_read[i], xq_write[i], "Q sample should match");
    }

    iqr_close(reader);
    cleanup_test_file();
    PASS();
}

TEST(iqr_reader_seek_rewind) {
    cleanup_test_file();

    /* Create recording */
    iqr_recorder_t *rec = NULL;
    iqr_create(&rec, 0);
    iqr_start(rec, TEST_FILENAME, 2000000.0, 5000000.0, 600, 40, 3);

    int16_t xi[1000], xq[1000];
    for (int i = 0; i < 1000; i++) {
        xi[i] = (int16_t)i;
        xq[i] = (int16_t)(1000 - i);
    }
    iqr_write(rec, xi, xq, 1000);
    iqr_stop(rec);
    iqr_destroy(rec);

    /* Test seek and rewind */
    iqr_reader_t *reader = NULL;
    iqr_open(&reader, TEST_FILENAME);

    /* Seek to sample 500 */
    iqr_error_t err = iqr_seek(reader, 500);
    ASSERT_EQ(err, IQR_OK, "seek should succeed");

    int16_t xi_read[10], xq_read[10];
    uint32_t num_read;
    iqr_read(reader, xi_read, xq_read, 10, &num_read);
    ASSERT_EQ(xi_read[0], 500, "first sample after seek should be 500");

    /* Rewind */
    err = iqr_rewind(reader);
    ASSERT_EQ(err, IQR_OK, "rewind should succeed");

    iqr_read(reader, xi_read, xq_read, 10, &num_read);
    ASSERT_EQ(xi_read[0], 0, "first sample after rewind should be 0");

    iqr_close(reader);
    cleanup_test_file();
    PASS();
}

/*============================================================================
 * Header Size Test
 *============================================================================*/

TEST(iqr_header_size) {
    /* Verify header is exactly 64 bytes */
    ASSERT_EQ(sizeof(iqr_header_t), IQR_HEADER_SIZE, "header should be 64 bytes");
    PASS();
}

/*============================================================================
 * Main
 *============================================================================*/

int main(void) {
    TEST_BEGIN("IQ Recorder Unit Tests");

    TEST_SECTION("Error Strings");
    RUN_TEST(iqr_strerror_codes);

    TEST_SECTION("Recorder Lifecycle");
    RUN_TEST(iqr_create_destroy);
    RUN_TEST(iqr_create_custom_buffer);
    RUN_TEST(iqr_create_null_ptr);
    RUN_TEST(iqr_destroy_null);

    TEST_SECTION("Recording");
    RUN_TEST(iqr_start_stop);
    RUN_TEST(iqr_double_start);
    RUN_TEST(iqr_stop_not_recording);
    RUN_TEST(iqr_write_samples);
    RUN_TEST(iqr_write_not_recording);
    RUN_TEST(iqr_duration);

    TEST_SECTION("Reader");
    RUN_TEST(iqr_reader_open_close);
    RUN_TEST(iqr_reader_open_nonexistent);
    RUN_TEST(iqr_reader_close_null);
    RUN_TEST(iqr_reader_header);
    RUN_TEST(iqr_reader_read_samples);
    RUN_TEST(iqr_reader_seek_rewind);

    TEST_SECTION("File Format");
    RUN_TEST(iqr_header_size);

    TEST_END();
    return TEST_EXIT_CODE();
}
