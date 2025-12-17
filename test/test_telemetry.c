/**
 * @file test_telemetry.c
 * @brief Unit test for waterfall_telemetry module
 *
 * Build: gcc -o test_telemetry test_telemetry.c waterfall_telemetry.c -lws2_32
 * Run:   test_telemetry.exe
 * 
 * Listen with: nc -u -l 3005  (or PowerShell UDP listener)
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "../tools/waterfall_telemetry.h"

#ifdef _WIN32
#include <windows.h>
#define sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define sleep_ms(ms) usleep((ms) * 1000)
#endif

static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST(name) printf("  TEST: %s ... ", name)
#define PASS() do { printf("PASS\n"); g_tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); g_tests_failed++; } while(0)

/*============================================================================
 * Test Cases
 *============================================================================*/

void test_init_cleanup(void) {
    TEST("init/cleanup");
    
    /* Should succeed */
    if (!telem_init(3005)) {
        FAIL("telem_init failed");
        return;
    }
    
    /* Double init should be ok */
    if (!telem_init(3005)) {
        FAIL("double init failed");
        telem_cleanup();
        return;
    }
    
    telem_cleanup();
    PASS();
}

void test_channel_enable_disable(void) {
    TEST("channel enable/disable");
    
    telem_init(3005);
    
    /* Initially none enabled */
    if (telem_get_channels() != TELEM_NONE) {
        FAIL("channels not initially NONE");
        telem_cleanup();
        return;
    }
    
    /* Enable single channel */
    telem_enable(TELEM_CHANNEL);
    if (!telem_is_enabled(TELEM_CHANNEL)) {
        FAIL("TELEM_CHANNEL not enabled");
        telem_cleanup();
        return;
    }
    
    /* Enable multiple */
    telem_enable(TELEM_TICKS | TELEM_MARKERS);
    if (!telem_is_enabled(TELEM_TICKS) || !telem_is_enabled(TELEM_MARKERS)) {
        FAIL("multi-enable failed");
        telem_cleanup();
        return;
    }
    
    /* Disable one */
    telem_disable(TELEM_TICKS);
    if (telem_is_enabled(TELEM_TICKS)) {
        FAIL("disable failed");
        telem_cleanup();
        return;
    }
    if (!telem_is_enabled(TELEM_CHANNEL)) {
        FAIL("disable affected other channel");
        telem_cleanup();
        return;
    }
    
    /* Set replaces all */
    telem_set_channels(TELEM_SYNC);
    if (telem_is_enabled(TELEM_CHANNEL) || !telem_is_enabled(TELEM_SYNC)) {
        FAIL("set_channels failed");
        telem_cleanup();
        return;
    }
    
    telem_cleanup();
    PASS();
}

void test_channel_prefixes(void) {
    TEST("channel prefixes");
    
    if (strcmp(telem_channel_prefix(TELEM_CHANNEL), "CHAN") != 0) {
        FAIL("TELEM_CHANNEL prefix wrong");
        return;
    }
    if (strcmp(telem_channel_prefix(TELEM_TICKS), "TICK") != 0) {
        FAIL("TELEM_TICKS prefix wrong");
        return;
    }
    if (strcmp(telem_channel_prefix(TELEM_MARKERS), "MARK") != 0) {
        FAIL("TELEM_MARKERS prefix wrong");
        return;
    }
    if (strcmp(telem_channel_prefix(TELEM_CARRIER), "CARR") != 0) {
        FAIL("TELEM_CARRIER prefix wrong");
        return;
    }
    if (strcmp(telem_channel_prefix(TELEM_SYNC), "SYNC") != 0) {
        FAIL("TELEM_SYNC prefix wrong");
        return;
    }
    
    PASS();
}

void test_send_disabled(void) {
    TEST("send when disabled (no crash, counted as dropped)");
    
    telem_init(3005);
    telem_set_channels(TELEM_NONE);
    
    uint32_t sent_before, dropped_before;
    telem_get_stats(&sent_before, &dropped_before);
    
    /* Send to disabled channel */
    telem_send(TELEM_CHANNEL, "test,data,here");
    telem_sendf(TELEM_TICKS, "%d,%f,%s", 42, 3.14, "hello");
    
    uint32_t sent_after, dropped_after;
    telem_get_stats(&sent_after, &dropped_after);
    
    if (sent_after != sent_before) {
        FAIL("sent count increased for disabled channel");
        telem_cleanup();
        return;
    }
    if (dropped_after != dropped_before + 2) {
        FAIL("dropped count not incremented");
        telem_cleanup();
        return;
    }
    
    telem_cleanup();
    PASS();
}

void test_send_enabled(void) {
    TEST("send when enabled (fires, counted as sent)");
    
    telem_init(3005);
    telem_enable(TELEM_CHANNEL | TELEM_TICKS);
    
    uint32_t sent_before, dropped_before;
    telem_get_stats(&sent_before, &dropped_before);
    
    /* Send to enabled channels */
    telem_send(TELEM_CHANNEL, "12:34:56,1000.0,45.2,32.1,-85.3");
    telem_sendf(TELEM_TICKS, "12:34:57,1001.0,%d,%.6f,%.1f", 42, 0.00123, 5.0);
    
    uint32_t sent_after, dropped_after;
    telem_get_stats(&sent_after, &dropped_after);
    
    if (sent_after != sent_before + 2) {
        FAIL("sent count not incremented");
        telem_cleanup();
        return;
    }
    if (dropped_after != dropped_before) {
        FAIL("dropped count changed unexpectedly");
        telem_cleanup();
        return;
    }
    
    telem_cleanup();
    PASS();
}

void test_edge_cases(void) {
    TEST("edge cases (NULL, empty, long strings)");
    
    telem_init(3005);
    telem_enable(TELEM_ALL);
    
    /* NULL should not crash */
    telem_send(TELEM_CHANNEL, NULL);
    
    /* Empty string should not crash */
    telem_send(TELEM_CHANNEL, "");
    
    /* Very long string should be truncated, not crash */
    char long_str[1024];
    memset(long_str, 'X', sizeof(long_str) - 1);
    long_str[sizeof(long_str) - 1] = '\0';
    telem_send(TELEM_CHANNEL, long_str);
    
    /* String with newline already */
    telem_send(TELEM_CHANNEL, "data,with,newline\n");
    
    /* String without newline */
    telem_send(TELEM_CHANNEL, "data,without,newline");
    
    telem_cleanup();
    PASS();
}

void test_broadcast_live(void) {
    TEST("live broadcast (check with UDP listener on port 3005)");
    
    telem_init(3005);
    telem_enable(TELEM_ALL);
    
    printf("\n    Sending 5 messages (listen with: nc -u -l 3005)\n");
    
    for (int i = 0; i < 5; i++) {
        telem_sendf(TELEM_CHANNEL, "test_msg,%d,%.2f,hello", i, i * 1.5);
        sleep_ms(200);
    }
    
    uint32_t sent, dropped;
    telem_get_stats(&sent, &dropped);
    printf("    Stats: sent=%u dropped=%u\n", sent, dropped);
    
    telem_cleanup();
    PASS();
}

/*============================================================================
 * Main
 *============================================================================*/

int main(int argc, char *argv[]) {
    printf("=== Telemetry Module Unit Tests ===\n\n");
    
    test_init_cleanup();
    test_channel_enable_disable();
    test_channel_prefixes();
    test_send_disabled();
    test_send_enabled();
    test_edge_cases();
    
    /* Only run live test if requested */
    if (argc > 1 && strcmp(argv[1], "--live") == 0) {
        test_broadcast_live();
    } else {
        printf("  (skipping live broadcast test, use --live to enable)\n");
    }
    
    printf("\n=== Results: %d passed, %d failed ===\n", 
           g_tests_passed, g_tests_failed);
    
    return g_tests_failed > 0 ? 1 : 0;
}
