/**
 * @file sdr_stubs.c
 * @brief Stub implementations of Phoenix SDR functions for unit testing
 *
 * These stubs allow tcp_commands.c to be linked without the actual SDR library.
 * All functions return success or neutral values.
 */

#include "phoenix_sdr.h"
#include <string.h>

/*============================================================================
 * Stub Implementations
 *============================================================================*/

const char* psdr_strerror(psdr_error_t err) {
    switch (err) {
        case PSDR_OK: return "OK";
        case PSDR_ERR_API_OPEN: return "API open failed";
        case PSDR_ERR_NOT_INITIALIZED: return "Not initialized";
        case PSDR_ERR_ALREADY_STREAMING: return "Already streaming";
        case PSDR_ERR_NOT_STREAMING: return "Not streaming";
        default: return "Unknown error";
    }
}

void psdr_config_defaults(psdr_config_t *config) {
    if (!config) return;
    memset(config, 0, sizeof(*config));
    config->freq_hz = 15000000.0;
    config->sample_rate_hz = 2000000.0;
    config->bandwidth = PSDR_BW_200;
    config->antenna = PSDR_ANT_A;
    config->agc_mode = PSDR_AGC_DISABLED;
    config->gain_reduction = 40;
    config->lna_state = 4;
    config->decimation = 1;
    config->bias_t = false;
    config->rf_notch = false;
    config->if_mode = PSDR_IF_ZERO;
    config->dc_offset_corr = true;
    config->iq_imbalance_corr = true;
    config->agc_setpoint_dbfs = -60;
}

psdr_error_t psdr_enumerate(psdr_device_info_t *devices, size_t max_devices, size_t *num_found) {
    (void)devices;
    (void)max_devices;
    if (num_found) *num_found = 0;
    return PSDR_ERR_NO_DEVICES;  /* No devices in test mode */
}

psdr_error_t psdr_open(psdr_context_t **ctx, unsigned int device_idx) {
    (void)device_idx;
    if (ctx) *ctx = NULL;
    return PSDR_ERR_NO_DEVICES;
}

psdr_error_t psdr_configure(psdr_context_t *ctx, const psdr_config_t *config) {
    (void)ctx;
    (void)config;
    return PSDR_OK;
}

psdr_error_t psdr_start(psdr_context_t *ctx, const psdr_callbacks_t *callbacks) {
    (void)ctx;
    (void)callbacks;
    return PSDR_OK;
}

psdr_error_t psdr_update(psdr_context_t *ctx, const psdr_config_t *config) {
    (void)ctx;
    (void)config;
    return PSDR_OK;
}

psdr_error_t psdr_stop(psdr_context_t *ctx) {
    (void)ctx;
    return PSDR_OK;
}

void psdr_close(psdr_context_t *ctx) {
    (void)ctx;
}

double psdr_get_sample_rate(const psdr_context_t *ctx) {
    (void)ctx;
    return 0.0;  /* Not streaming */
}

bool psdr_is_streaming(const psdr_context_t *ctx) {
    (void)ctx;
    return false;
}

void psdr_print_device_params(const psdr_context_t *ctx) {
    (void)ctx;
}
