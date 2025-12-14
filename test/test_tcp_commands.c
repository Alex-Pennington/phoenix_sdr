/**
 * @file test_tcp_commands.c
 * @brief Unit tests for TCP command parser and executor
 *
 * Tests each command to verify parsing and execution work correctly.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "tcp_server.h"
#include "version.h"

/*============================================================================
 * Test Framework
 *============================================================================*/

static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("  Testing: %s... ", #name); \
    test_##name(); \
    g_tests_run++; \
} while(0)

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL\n    Assertion failed: %s\n    %s\n", #cond, msg); \
        g_tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_EQ(a, b, msg) ASSERT((a) == (b), msg)
#define ASSERT_STR_EQ(a, b, msg) ASSERT(strcmp((a), (b)) == 0, msg)
#define ASSERT_FLOAT_EQ(a, b, eps, msg) ASSERT(fabs((a) - (b)) < (eps), msg)

#define PASS() do { printf("PASS\n"); g_tests_passed++; } while(0)

/*============================================================================
 * Parser Tests
 *============================================================================*/

TEST(parse_empty_line) {
    tcp_command_t cmd;
    tcp_error_t err;

    err = tcp_parse_command("", &cmd);
    ASSERT_EQ(err, TCP_ERR_SYNTAX, "empty line should fail");

    err = tcp_parse_command("\n", &cmd);
    ASSERT_EQ(err, TCP_ERR_SYNTAX, "newline only should fail");

    err = tcp_parse_command("   \n", &cmd);
    ASSERT_EQ(err, TCP_ERR_SYNTAX, "whitespace only should fail");

    PASS();
}

TEST(parse_unknown_command) {
    tcp_command_t cmd;
    tcp_error_t err;

    err = tcp_parse_command("FOOBAR\n", &cmd);
    ASSERT_EQ(err, TCP_ERR_UNKNOWN, "unknown command should return UNKNOWN");

    err = tcp_parse_command("SET_FOOBAR 123\n", &cmd);
    ASSERT_EQ(err, TCP_ERR_UNKNOWN, "unknown SET_ command should return UNKNOWN");

    PASS();
}

TEST(parse_case_insensitive) {
    tcp_command_t cmd;
    tcp_error_t err;

    err = tcp_parse_command("ping\n", &cmd);
    ASSERT_EQ(err, TCP_OK, "lowercase ping should parse");
    ASSERT_EQ(cmd.type, CMD_PING, "should be PING command");

    err = tcp_parse_command("Ping\n", &cmd);
    ASSERT_EQ(err, TCP_OK, "mixed case Ping should parse");

    err = tcp_parse_command("SET_FREQ 1000000\n", &cmd);
    ASSERT_EQ(err, TCP_OK, "uppercase should parse");

    err = tcp_parse_command("set_freq 1000000\n", &cmd);
    ASSERT_EQ(err, TCP_OK, "lowercase should parse");

    PASS();
}

/*============================================================================
 * PING/VER/QUIT Tests
 *============================================================================*/

TEST(cmd_ping) {
    tcp_command_t cmd;
    tcp_response_t resp;
    tcp_sdr_state_t state;

    tcp_state_defaults(&state);

    ASSERT_EQ(tcp_parse_command("PING\n", &cmd), TCP_OK, "PING should parse");
    ASSERT_EQ(cmd.type, CMD_PING, "should be PING command");
    ASSERT_EQ(cmd.argc, 0, "PING has no args");

    tcp_execute_command(&cmd, &state, &resp);
    ASSERT_STR_EQ(resp.message, "PONG", "response should be PONG");

    /* PING with args should fail */
    ASSERT_EQ(tcp_parse_command("PING extra\n", &cmd), TCP_ERR_SYNTAX,
        "PING with args should fail");

    PASS();
}

TEST(cmd_ver) {
    tcp_command_t cmd;
    tcp_response_t resp;
    tcp_sdr_state_t state;

    tcp_state_defaults(&state);

    ASSERT_EQ(tcp_parse_command("VER\n", &cmd), TCP_OK, "VER should parse");
    ASSERT_EQ(cmd.type, CMD_VER, "should be VER command");

    tcp_execute_command(&cmd, &state, &resp);
    ASSERT(strstr(resp.message, "PHOENIX_SDR=") != NULL, "should contain PHOENIX_SDR=");
    ASSERT(strstr(resp.message, "PROTOCOL=1.0") != NULL, "should contain PROTOCOL=1.0");

    PASS();
}

TEST(cmd_quit) {
    tcp_command_t cmd;
    tcp_response_t resp;
    tcp_sdr_state_t state;

    tcp_state_defaults(&state);

    ASSERT_EQ(tcp_parse_command("QUIT\n", &cmd), TCP_OK, "QUIT should parse");
    ASSERT_EQ(cmd.type, CMD_QUIT, "should be QUIT command");

    tcp_execute_command(&cmd, &state, &resp);
    ASSERT_STR_EQ(resp.message, "BYE", "response should be BYE");

    PASS();
}

TEST(cmd_help) {
    tcp_command_t cmd;
    tcp_response_t resp;
    tcp_sdr_state_t state;

    tcp_state_defaults(&state);

    ASSERT_EQ(tcp_parse_command("HELP\n", &cmd), TCP_OK, "HELP should parse");

    tcp_execute_command(&cmd, &state, &resp);
    ASSERT(strstr(resp.message, "OK COMMANDS:") != NULL, "should start with OK COMMANDS:");
    ASSERT(strstr(resp.message, "SET_FREQ") != NULL, "should list SET_FREQ");
    ASSERT(strstr(resp.message, "QUIT") != NULL, "should list QUIT");

    PASS();
}

/*============================================================================
 * SET_FREQ / GET_FREQ Tests
 *============================================================================*/

TEST(cmd_set_freq_valid) {
    tcp_command_t cmd;
    tcp_response_t resp;
    tcp_sdr_state_t state;

    tcp_state_defaults(&state);

    /* Valid frequency */
    ASSERT_EQ(tcp_parse_command("SET_FREQ 15000000\n", &cmd), TCP_OK, "should parse");
    ASSERT_EQ(cmd.type, CMD_SET_FREQ, "should be SET_FREQ");
    ASSERT_FLOAT_EQ(cmd.value.freq_hz, 15000000.0, 1.0, "freq should be 15MHz");

    tcp_execute_command(&cmd, &state, &resp);
    ASSERT_STR_EQ(resp.message, "OK", "response should be OK");
    ASSERT_FLOAT_EQ(state.freq_hz, 15000000.0, 1.0, "state should be updated");

    /* Another valid frequency */
    ASSERT_EQ(tcp_parse_command("SET_FREQ 7255000\n", &cmd), TCP_OK, "7.255 MHz should parse");
    tcp_execute_command(&cmd, &state, &resp);
    ASSERT_FLOAT_EQ(state.freq_hz, 7255000.0, 1.0, "state should be 7.255 MHz");

    /* Floating point frequency */
    ASSERT_EQ(tcp_parse_command("SET_FREQ 14100500.5\n", &cmd), TCP_OK, "float freq should parse");
    ASSERT_FLOAT_EQ(cmd.value.freq_hz, 14100500.5, 1.0, "should handle float");

    PASS();
}

TEST(cmd_set_freq_range) {
    tcp_command_t cmd;

    /* Too low */
    ASSERT_EQ(tcp_parse_command("SET_FREQ 500\n", &cmd), TCP_ERR_RANGE,
        "500 Hz should be out of range");

    /* Minimum valid */
    ASSERT_EQ(tcp_parse_command("SET_FREQ 1000\n", &cmd), TCP_OK,
        "1000 Hz should be valid");

    /* Maximum valid */
    ASSERT_EQ(tcp_parse_command("SET_FREQ 2000000000\n", &cmd), TCP_OK,
        "2 GHz should be valid");

    /* Too high */
    ASSERT_EQ(tcp_parse_command("SET_FREQ 2000000001\n", &cmd), TCP_ERR_RANGE,
        ">2 GHz should be out of range");

    PASS();
}

TEST(cmd_set_freq_missing_arg) {
    tcp_command_t cmd;

    ASSERT_EQ(tcp_parse_command("SET_FREQ\n", &cmd), TCP_ERR_SYNTAX,
        "SET_FREQ without arg should fail");

    PASS();
}

TEST(cmd_get_freq) {
    tcp_command_t cmd;
    tcp_response_t resp;
    tcp_sdr_state_t state;

    tcp_state_defaults(&state);
    state.freq_hz = 14250000.0;

    ASSERT_EQ(tcp_parse_command("GET_FREQ\n", &cmd), TCP_OK, "should parse");
    ASSERT_EQ(cmd.type, CMD_GET_FREQ, "should be GET_FREQ");

    tcp_execute_command(&cmd, &state, &resp);
    ASSERT_STR_EQ(resp.message, "OK 14250000", "should return frequency");

    PASS();
}

/*============================================================================
 * SET_GAIN / GET_GAIN Tests
 *============================================================================*/

TEST(cmd_set_gain_valid) {
    tcp_command_t cmd;
    tcp_response_t resp;
    tcp_sdr_state_t state;

    tcp_state_defaults(&state);

    ASSERT_EQ(tcp_parse_command("SET_GAIN 40\n", &cmd), TCP_OK, "should parse");
    ASSERT_EQ(cmd.value.gain_db, 40, "gain should be 40");

    tcp_execute_command(&cmd, &state, &resp);
    ASSERT_EQ(state.gain_reduction, 40, "state should be updated");

    PASS();
}

TEST(cmd_set_gain_range) {
    tcp_command_t cmd;

    /* Too low */
    ASSERT_EQ(tcp_parse_command("SET_GAIN 19\n", &cmd), TCP_ERR_RANGE,
        "gain 19 should be out of range");

    /* Min valid */
    ASSERT_EQ(tcp_parse_command("SET_GAIN 20\n", &cmd), TCP_OK,
        "gain 20 should be valid");

    /* Max valid */
    ASSERT_EQ(tcp_parse_command("SET_GAIN 59\n", &cmd), TCP_OK,
        "gain 59 should be valid");

    /* Too high */
    ASSERT_EQ(tcp_parse_command("SET_GAIN 60\n", &cmd), TCP_ERR_RANGE,
        "gain 60 should be out of range");

    PASS();
}

TEST(cmd_get_gain) {
    tcp_command_t cmd;
    tcp_response_t resp;
    tcp_sdr_state_t state;

    tcp_state_defaults(&state);
    state.gain_reduction = 35;

    ASSERT_EQ(tcp_parse_command("GET_GAIN\n", &cmd), TCP_OK, "should parse");

    tcp_execute_command(&cmd, &state, &resp);
    ASSERT_STR_EQ(resp.message, "OK 35", "should return gain");

    PASS();
}

/*============================================================================
 * SET_LNA / GET_LNA Tests
 *============================================================================*/

TEST(cmd_set_lna_valid) {
    tcp_command_t cmd;
    tcp_response_t resp;
    tcp_sdr_state_t state;

    tcp_state_defaults(&state);

    ASSERT_EQ(tcp_parse_command("SET_LNA 5\n", &cmd), TCP_OK, "should parse");
    ASSERT_EQ(cmd.value.lna_state, 5, "LNA should be 5");

    tcp_execute_command(&cmd, &state, &resp);
    ASSERT_EQ(state.lna_state, 5, "state should be updated");

    PASS();
}

TEST(cmd_set_lna_range) {
    tcp_command_t cmd;

    ASSERT_EQ(tcp_parse_command("SET_LNA 0\n", &cmd), TCP_OK, "LNA 0 valid");
    ASSERT_EQ(tcp_parse_command("SET_LNA 8\n", &cmd), TCP_OK, "LNA 8 valid");
    ASSERT_EQ(tcp_parse_command("SET_LNA 9\n", &cmd), TCP_ERR_RANGE, "LNA 9 invalid");
    ASSERT_EQ(tcp_parse_command("SET_LNA -1\n", &cmd), TCP_ERR_RANGE, "LNA -1 invalid");

    PASS();
}

/*============================================================================
 * SET_AGC / GET_AGC Tests
 *============================================================================*/

TEST(cmd_set_agc_valid) {
    tcp_command_t cmd;
    tcp_response_t resp;
    tcp_sdr_state_t state;

    tcp_state_defaults(&state);

    ASSERT_EQ(tcp_parse_command("SET_AGC OFF\n", &cmd), TCP_OK, "OFF should parse");
    tcp_execute_command(&cmd, &state, &resp);
    ASSERT_STR_EQ(state.agc_mode, "OFF", "state should be OFF");

    ASSERT_EQ(tcp_parse_command("SET_AGC 5HZ\n", &cmd), TCP_OK, "5HZ should parse");
    tcp_execute_command(&cmd, &state, &resp);
    ASSERT_STR_EQ(state.agc_mode, "5HZ", "state should be 5HZ");

    ASSERT_EQ(tcp_parse_command("SET_AGC 50HZ\n", &cmd), TCP_OK, "50HZ should parse");
    ASSERT_EQ(tcp_parse_command("SET_AGC 100HZ\n", &cmd), TCP_OK, "100HZ should parse");

    /* Case insensitive */
    ASSERT_EQ(tcp_parse_command("SET_AGC off\n", &cmd), TCP_OK, "lowercase off should parse");

    PASS();
}

TEST(cmd_set_agc_invalid) {
    tcp_command_t cmd;

    ASSERT_EQ(tcp_parse_command("SET_AGC AUTO\n", &cmd), TCP_ERR_PARAM,
        "AUTO should be invalid");
    ASSERT_EQ(tcp_parse_command("SET_AGC FAST\n", &cmd), TCP_ERR_PARAM,
        "FAST should be invalid");
    ASSERT_EQ(tcp_parse_command("SET_AGC 10HZ\n", &cmd), TCP_ERR_PARAM,
        "10HZ should be invalid");

    PASS();
}

/*============================================================================
 * SET_SRATE / GET_SRATE Tests
 *============================================================================*/

TEST(cmd_set_srate_valid) {
    tcp_command_t cmd;
    tcp_response_t resp;
    tcp_sdr_state_t state;

    tcp_state_defaults(&state);

    ASSERT_EQ(tcp_parse_command("SET_SRATE 2000000\n", &cmd), TCP_OK, "2M should parse");
    ASSERT_EQ(cmd.value.sample_rate, 2000000, "should be 2M");

    tcp_execute_command(&cmd, &state, &resp);
    ASSERT_EQ(state.sample_rate, 2000000, "state should be updated");

    PASS();
}

TEST(cmd_set_srate_while_streaming) {
    tcp_command_t cmd;
    tcp_response_t resp;
    tcp_sdr_state_t state;

    tcp_state_defaults(&state);
    state.streaming = true;  /* Simulate streaming */

    ASSERT_EQ(tcp_parse_command("SET_SRATE 6000000\n", &cmd), TCP_OK, "should parse");

    tcp_error_t err = tcp_execute_command(&cmd, &state, &resp);
    ASSERT_EQ(err, TCP_ERR_STATE, "should fail while streaming");
    ASSERT(strstr(resp.message, "ERR STATE") != NULL, "error message should indicate STATE");

    PASS();
}

/*============================================================================
 * SET_BW / GET_BW Tests
 *============================================================================*/

TEST(cmd_set_bw_valid) {
    tcp_command_t cmd;

    ASSERT_EQ(tcp_parse_command("SET_BW 200\n", &cmd), TCP_OK, "200 kHz valid");
    ASSERT_EQ(tcp_parse_command("SET_BW 300\n", &cmd), TCP_OK, "300 kHz valid");
    ASSERT_EQ(tcp_parse_command("SET_BW 600\n", &cmd), TCP_OK, "600 kHz valid");
    ASSERT_EQ(tcp_parse_command("SET_BW 1536\n", &cmd), TCP_OK, "1536 kHz valid");
    ASSERT_EQ(tcp_parse_command("SET_BW 5000\n", &cmd), TCP_OK, "5000 kHz valid");
    ASSERT_EQ(tcp_parse_command("SET_BW 8000\n", &cmd), TCP_OK, "8000 kHz valid");

    PASS();
}

TEST(cmd_set_bw_invalid) {
    tcp_command_t cmd;

    ASSERT_EQ(tcp_parse_command("SET_BW 100\n", &cmd), TCP_ERR_PARAM, "100 kHz invalid");
    ASSERT_EQ(tcp_parse_command("SET_BW 500\n", &cmd), TCP_ERR_PARAM, "500 kHz invalid");
    ASSERT_EQ(tcp_parse_command("SET_BW 1000\n", &cmd), TCP_ERR_PARAM, "1000 kHz invalid");

    PASS();
}

/*============================================================================
 * SET_ANTENNA / GET_ANTENNA Tests
 *============================================================================*/

TEST(cmd_set_antenna_valid) {
    tcp_command_t cmd;
    tcp_response_t resp;
    tcp_sdr_state_t state;

    tcp_state_defaults(&state);

    ASSERT_EQ(tcp_parse_command("SET_ANTENNA A\n", &cmd), TCP_OK, "A valid");
    tcp_execute_command(&cmd, &state, &resp);
    ASSERT_STR_EQ(state.antenna, "A", "state should be A");

    ASSERT_EQ(tcp_parse_command("SET_ANTENNA B\n", &cmd), TCP_OK, "B valid");
    ASSERT_EQ(tcp_parse_command("SET_ANTENNA HIZ\n", &cmd), TCP_OK, "HIZ valid");
    ASSERT_EQ(tcp_parse_command("SET_ANTENNA hiz\n", &cmd), TCP_OK, "lowercase hiz valid");

    PASS();
}

TEST(cmd_set_antenna_invalid) {
    tcp_command_t cmd;

    ASSERT_EQ(tcp_parse_command("SET_ANTENNA C\n", &cmd), TCP_ERR_PARAM, "C invalid");
    ASSERT_EQ(tcp_parse_command("SET_ANTENNA 1\n", &cmd), TCP_ERR_PARAM, "1 invalid");

    PASS();
}

/*============================================================================
 * SET_BIAST Tests
 *============================================================================*/

TEST(cmd_set_biast_safety) {
    tcp_command_t cmd;
    tcp_response_t resp;
    tcp_sdr_state_t state;

    tcp_state_defaults(&state);

    /* OFF should work without CONFIRM */
    ASSERT_EQ(tcp_parse_command("SET_BIAST OFF\n", &cmd), TCP_OK, "OFF should parse");
    tcp_execute_command(&cmd, &state, &resp);
    ASSERT_EQ(state.bias_t, false, "should be OFF");
    ASSERT_STR_EQ(resp.message, "OK", "should succeed");

    /* ON without CONFIRM should fail */
    ASSERT_EQ(tcp_parse_command("SET_BIAST ON\n", &cmd), TCP_OK, "ON should parse");
    tcp_error_t err = tcp_execute_command(&cmd, &state, &resp);
    ASSERT_EQ(err, TCP_ERR_PARAM, "ON without CONFIRM should fail");
    ASSERT_EQ(state.bias_t, false, "state should still be OFF");

    /* ON with CONFIRM should work */
    ASSERT_EQ(tcp_parse_command("SET_BIAST ON CONFIRM\n", &cmd), TCP_OK, "ON CONFIRM should parse");
    tcp_execute_command(&cmd, &state, &resp);
    ASSERT_EQ(state.bias_t, true, "should be ON");

    PASS();
}

/*============================================================================
 * START / STOP Tests
 *============================================================================*/

TEST(cmd_start_stop) {
    tcp_command_t cmd;
    tcp_response_t resp;
    tcp_sdr_state_t state;

    tcp_state_defaults(&state);
    ASSERT_EQ(state.streaming, false, "initially not streaming");

    /* Start */
    ASSERT_EQ(tcp_parse_command("START\n", &cmd), TCP_OK, "START should parse");
    tcp_execute_command(&cmd, &state, &resp);
    ASSERT_EQ(state.streaming, true, "should be streaming");
    ASSERT_STR_EQ(resp.message, "OK", "should succeed");

    /* Start again should fail */
    tcp_error_t err = tcp_execute_command(&cmd, &state, &resp);
    ASSERT_EQ(err, TCP_ERR_STATE, "double START should fail");

    /* Stop */
    ASSERT_EQ(tcp_parse_command("STOP\n", &cmd), TCP_OK, "STOP should parse");
    tcp_execute_command(&cmd, &state, &resp);
    ASSERT_EQ(state.streaming, false, "should not be streaming");

    /* Stop again should fail */
    err = tcp_execute_command(&cmd, &state, &resp);
    ASSERT_EQ(err, TCP_ERR_STATE, "double STOP should fail");

    PASS();
}

/*============================================================================
 * STATUS Tests
 *============================================================================*/

TEST(cmd_status) {
    tcp_command_t cmd;
    tcp_response_t resp;
    tcp_sdr_state_t state;

    tcp_state_defaults(&state);
    state.freq_hz = 14250000.0;
    state.gain_reduction = 35;
    state.lna_state = 3;

    ASSERT_EQ(tcp_parse_command("STATUS\n", &cmd), TCP_OK, "STATUS should parse");
    tcp_execute_command(&cmd, &state, &resp);

    ASSERT(strstr(resp.message, "OK") != NULL, "should start with OK");
    ASSERT(strstr(resp.message, "STREAMING=0") != NULL, "should have STREAMING=0");
    ASSERT(strstr(resp.message, "FREQ=14250000") != NULL, "should have frequency");
    ASSERT(strstr(resp.message, "GAIN=35") != NULL, "should have gain");
    ASSERT(strstr(resp.message, "LNA=3") != NULL, "should have LNA");

    /* When streaming, should also show OVERLOAD */
    state.streaming = true;
    tcp_execute_command(&cmd, &state, &resp);
    ASSERT(strstr(resp.message, "STREAMING=1") != NULL, "should have STREAMING=1");
    ASSERT(strstr(resp.message, "OVERLOAD=") != NULL, "should have OVERLOAD status");

    PASS();
}

/*============================================================================
 * Response Formatting Tests
 *============================================================================*/

TEST(response_formatting) {
    tcp_response_t resp;
    char buf[256];

    tcp_response_ok(&resp, NULL);
    tcp_format_response(&resp, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "OK\n", "OK with no value");

    tcp_response_ok(&resp, "123456");
    tcp_format_response(&resp, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "OK 123456\n", "OK with value");

    tcp_response_error(&resp, TCP_ERR_RANGE, "value too high");
    tcp_format_response(&resp, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "ERR RANGE value too high\n", "error with message");

    tcp_response_error(&resp, TCP_ERR_UNKNOWN, NULL);
    tcp_format_response(&resp, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "ERR UNKNOWN\n", "error without message");

    PASS();
}

/*============================================================================
 * Main
 *============================================================================*/

int main(void) {
    print_version("Phoenix SDR - TCP Command Unit Tests");
    printf("\n");

    printf("=== Parser Tests ===\n");
    RUN_TEST(parse_empty_line);
    RUN_TEST(parse_unknown_command);
    RUN_TEST(parse_case_insensitive);

    printf("\n=== Utility Commands ===\n");
    RUN_TEST(cmd_ping);
    RUN_TEST(cmd_ver);
    RUN_TEST(cmd_quit);
    RUN_TEST(cmd_help);

    printf("\n=== Frequency Commands ===\n");
    RUN_TEST(cmd_set_freq_valid);
    RUN_TEST(cmd_set_freq_range);
    RUN_TEST(cmd_set_freq_missing_arg);
    RUN_TEST(cmd_get_freq);

    printf("\n=== Gain Commands ===\n");
    RUN_TEST(cmd_set_gain_valid);
    RUN_TEST(cmd_set_gain_range);
    RUN_TEST(cmd_get_gain);

    printf("\n=== LNA Commands ===\n");
    RUN_TEST(cmd_set_lna_valid);
    RUN_TEST(cmd_set_lna_range);

    printf("\n=== AGC Commands ===\n");
    RUN_TEST(cmd_set_agc_valid);
    RUN_TEST(cmd_set_agc_invalid);

    printf("\n=== Sample Rate Commands ===\n");
    RUN_TEST(cmd_set_srate_valid);
    RUN_TEST(cmd_set_srate_while_streaming);

    printf("\n=== Bandwidth Commands ===\n");
    RUN_TEST(cmd_set_bw_valid);
    RUN_TEST(cmd_set_bw_invalid);

    printf("\n=== Antenna Commands ===\n");
    RUN_TEST(cmd_set_antenna_valid);
    RUN_TEST(cmd_set_antenna_invalid);

    printf("\n=== Bias-T Commands ===\n");
    RUN_TEST(cmd_set_biast_safety);

    printf("\n=== Streaming Commands ===\n");
    RUN_TEST(cmd_start_stop);

    printf("\n=== Status Command ===\n");
    RUN_TEST(cmd_status);

    printf("\n=== Response Formatting ===\n");
    RUN_TEST(response_formatting);

    /* Summary */
    printf("\n========================================\n");
    printf("Tests run:    %d\n", g_tests_run);
    printf("Tests passed: %d\n", g_tests_passed);
    printf("Tests failed: %d\n", g_tests_failed);
    printf("========================================\n");

    return (g_tests_failed > 0) ? 1 : 0;
}
