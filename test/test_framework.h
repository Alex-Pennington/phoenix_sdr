/**
 * @file test_framework.h
 * @brief Minimal unit test framework for Phoenix SDR
 *
 * Usage:
 *   #include "test_framework.h"
 *
 *   TEST(my_test_name) {
 *       ASSERT_EQ(1, 1, "one equals one");
 *       PASS();
 *   }
 *
 *   int main(void) {
 *       TEST_BEGIN("My Test Suite");
 *       RUN_TEST(my_test_name);
 *       TEST_END();
 *       return TEST_EXIT_CODE();
 *   }
 */

#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>

/*============================================================================
 * Global Test State
 *============================================================================*/

static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;
static int g_tests_skipped = 0;
static const char *g_current_suite = NULL;

/*============================================================================
 * Test Definition Macros
 *============================================================================*/

/** Define a test function */
#define TEST(name) static void test_##name(void)

/** Run a test and track results */
#define RUN_TEST(name) do { \
    printf("  [TEST] %s ... ", #name); \
    fflush(stdout); \
    test_##name(); \
    g_tests_run++; \
} while(0)

/** Skip a test (for platform-specific or disabled tests) */
#define SKIP_TEST(name, reason) do { \
    printf("  [SKIP] %s: %s\n", #name, reason); \
    g_tests_skipped++; \
} while(0)

/** Skip from inside a test function (early return) */
#define SKIP(reason) do { \
    printf("SKIP (%s)\n", reason); \
    g_tests_skipped++; \
    g_tests_run--; \
    return; \
} while(0)

/*============================================================================
 * Assertion Macros
 *============================================================================*/

/** Basic assertion with custom message */
#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL\n"); \
        printf("    Assertion failed: %s\n", #cond); \
        printf("    Message: %s\n", msg); \
        printf("    Location: %s:%d\n", __FILE__, __LINE__); \
        g_tests_failed++; \
        return; \
    } \
} while(0)

/** Assert equality (integers, pointers) */
#define ASSERT_EQ(actual, expected, msg) do { \
    long long _a = (long long)(actual); \
    long long _e = (long long)(expected); \
    if (_a != _e) { \
        printf("FAIL\n"); \
        printf("    ASSERT_EQ failed: %s\n", #actual " == " #expected); \
        printf("    Actual:   %lld\n", _a); \
        printf("    Expected: %lld\n", _e); \
        printf("    Message: %s\n", msg); \
        printf("    Location: %s:%d\n", __FILE__, __LINE__); \
        g_tests_failed++; \
        return; \
    } \
} while(0)

/** Assert inequality */
#define ASSERT_NE(actual, expected, msg) do { \
    long long _a = (long long)(actual); \
    long long _e = (long long)(expected); \
    if (_a == _e) { \
        printf("FAIL\n"); \
        printf("    ASSERT_NE failed: %s\n", #actual " != " #expected); \
        printf("    Both values: %lld\n", _a); \
        printf("    Message: %s\n", msg); \
        printf("    Location: %s:%d\n", __FILE__, __LINE__); \
        g_tests_failed++; \
        return; \
    } \
} while(0)

/** Assert greater than */
#define ASSERT_GT(actual, threshold, msg) do { \
    long long _a = (long long)(actual); \
    long long _t = (long long)(threshold); \
    if (_a <= _t) { \
        printf("FAIL\n"); \
        printf("    ASSERT_GT failed: %s\n", #actual " > " #threshold); \
        printf("    Actual:    %lld\n", _a); \
        printf("    Threshold: %lld\n", _t); \
        printf("    Message: %s\n", msg); \
        printf("    Location: %s:%d\n", __FILE__, __LINE__); \
        g_tests_failed++; \
        return; \
    } \
} while(0)

/** Assert less than */
#define ASSERT_LT(actual, threshold, msg) do { \
    long long _a = (long long)(actual); \
    long long _t = (long long)(threshold); \
    if (_a >= _t) { \
        printf("FAIL\n"); \
        printf("    ASSERT_LT failed: %s\n", #actual " < " #threshold); \
        printf("    Actual:    %lld\n", _a); \
        printf("    Threshold: %lld\n", _t); \
        printf("    Message: %s\n", msg); \
        printf("    Location: %s:%d\n", __FILE__, __LINE__); \
        g_tests_failed++; \
        return; \
    } \
} while(0)

/** Assert string equality */
#define ASSERT_STR_EQ(actual, expected, msg) do { \
    const char *_a = (actual); \
    const char *_e = (expected); \
    if (_a == NULL && _e == NULL) { /* both NULL is equal */ } \
    else if (_a == NULL || _e == NULL || strcmp(_a, _e) != 0) { \
        printf("FAIL\n"); \
        printf("    ASSERT_STR_EQ failed: %s\n", #actual " == " #expected); \
        printf("    Actual:   \"%s\"\n", _a ? _a : "(NULL)"); \
        printf("    Expected: \"%s\"\n", _e ? _e : "(NULL)"); \
        printf("    Message: %s\n", msg); \
        printf("    Location: %s:%d\n", __FILE__, __LINE__); \
        g_tests_failed++; \
        return; \
    } \
} while(0)

/** Assert string contains substring */
#define ASSERT_STR_CONTAINS(haystack, needle, msg) do { \
    const char *_h = (haystack); \
    const char *_n = (needle); \
    if (_h == NULL || _n == NULL || strstr(_h, _n) == NULL) { \
        printf("FAIL\n"); \
        printf("    ASSERT_STR_CONTAINS failed\n"); \
        printf("    Haystack: \"%s\"\n", _h ? _h : "(NULL)"); \
        printf("    Needle:   \"%s\"\n", _n ? _n : "(NULL)"); \
        printf("    Message: %s\n", msg); \
        printf("    Location: %s:%d\n", __FILE__, __LINE__); \
        g_tests_failed++; \
        return; \
    } \
} while(0)

/** Assert floating-point equality within epsilon */
#define ASSERT_FLOAT_EQ(actual, expected, epsilon, msg) do { \
    double _a = (double)(actual); \
    double _e = (double)(expected); \
    double _eps = (double)(epsilon); \
    if (fabs(_a - _e) >= _eps) { \
        printf("FAIL\n"); \
        printf("    ASSERT_FLOAT_EQ failed: %s\n", #actual " ~= " #expected); \
        printf("    Actual:   %g\n", _a); \
        printf("    Expected: %g\n", _e); \
        printf("    Epsilon:  %g\n", _eps); \
        printf("    Delta:    %g\n", fabs(_a - _e)); \
        printf("    Message: %s\n", msg); \
        printf("    Location: %s:%d\n", __FILE__, __LINE__); \
        g_tests_failed++; \
        return; \
    } \
} while(0)

/** Assert pointer is not NULL */
#define ASSERT_NOT_NULL(ptr, msg) do { \
    if ((ptr) == NULL) { \
        printf("FAIL\n"); \
        printf("    ASSERT_NOT_NULL failed: %s\n", #ptr); \
        printf("    Message: %s\n", msg); \
        printf("    Location: %s:%d\n", __FILE__, __LINE__); \
        g_tests_failed++; \
        return; \
    } \
} while(0)

/** Assert pointer is NULL */
#define ASSERT_NULL(ptr, msg) do { \
    if ((ptr) != NULL) { \
        printf("FAIL\n"); \
        printf("    ASSERT_NULL failed: %s\n", #ptr); \
        printf("    Actual: %p\n", (void*)(ptr)); \
        printf("    Message: %s\n", msg); \
        printf("    Location: %s:%d\n", __FILE__, __LINE__); \
        g_tests_failed++; \
        return; \
    } \
} while(0)

/** Assert boolean true */
#define ASSERT_TRUE(cond, msg) ASSERT((cond), msg)

/** Assert boolean false */
#define ASSERT_FALSE(cond, msg) ASSERT(!(cond), msg)

/** Mark test as passed (call at end of successful test) */
#define PASS() do { \
    printf("PASS\n"); \
    g_tests_passed++; \
} while(0)

/** Explicit fail with message */
#define FAIL(msg) do { \
    printf("FAIL\n"); \
    printf("    Explicit failure: %s\n", msg); \
    printf("    Location: %s:%d\n", __FILE__, __LINE__); \
    g_tests_failed++; \
    return; \
} while(0)

/*============================================================================
 * Test Suite Management
 *============================================================================*/

/** Begin a test suite */
#define TEST_BEGIN(suite_name) do { \
    g_current_suite = suite_name; \
    printf("\n========================================\n"); \
    printf("  %s\n", suite_name); \
    printf("========================================\n\n"); \
} while(0)

/** Print a section header within a test suite */
#define TEST_SECTION(section_name) do { \
    printf("\n--- %s ---\n", section_name); \
} while(0)

/** End a test suite and print summary */
#define TEST_END() do { \
    printf("\n========================================\n"); \
    printf("  Results: %s\n", g_current_suite ? g_current_suite : "Tests"); \
    printf("========================================\n"); \
    printf("  Run:     %d\n", g_tests_run); \
    printf("  Passed:  %d\n", g_tests_passed); \
    printf("  Failed:  %d\n", g_tests_failed); \
    printf("  Skipped: %d\n", g_tests_skipped); \
    printf("========================================\n"); \
    if (g_tests_failed > 0) { \
        printf("  STATUS: FAILED\n"); \
    } else { \
        printf("  STATUS: PASSED\n"); \
    } \
    printf("========================================\n\n"); \
} while(0)

/** Get exit code based on test results */
#define TEST_EXIT_CODE() (g_tests_failed > 0 ? 1 : 0)

/** Reset test state (for running multiple suites) */
#define TEST_RESET() do { \
    g_tests_run = 0; \
    g_tests_passed = 0; \
    g_tests_failed = 0; \
    g_tests_skipped = 0; \
    g_current_suite = NULL; \
} while(0)

/*============================================================================
 * Memory Testing Helpers
 *============================================================================*/

#ifdef _DEBUG
/** Simple memory tracking for leak detection (debug builds only) */
static size_t g_alloc_count = 0;
static size_t g_free_count = 0;

#define TRACK_ALLOC() (g_alloc_count++)
#define TRACK_FREE() (g_free_count++)
#define ASSERT_NO_LEAKS(msg) do { \
    if (g_alloc_count != g_free_count) { \
        printf("FAIL\n"); \
        printf("    Memory leak detected!\n"); \
        printf("    Allocations: %zu\n", g_alloc_count); \
        printf("    Frees: %zu\n", g_free_count); \
        printf("    Leaked: %zu\n", g_alloc_count - g_free_count); \
        printf("    Message: %s\n", msg); \
        g_tests_failed++; \
        return; \
    } \
} while(0)
#define RESET_ALLOC_TRACKING() do { g_alloc_count = 0; g_free_count = 0; } while(0)
#else
#define TRACK_ALLOC() ((void)0)
#define TRACK_FREE() ((void)0)
#define ASSERT_NO_LEAKS(msg) ((void)0)
#define RESET_ALLOC_TRACKING() ((void)0)
#endif

#endif /* TEST_FRAMEWORK_H */
