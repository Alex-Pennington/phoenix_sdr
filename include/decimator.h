/**
 * @file decimator.h
 * @brief Multi-stage decimation for SDR sample rate conversion
 * 
 * Converts 2 MSPS I/Q from SDRplay to 48kHz complex baseband for modem input.
 * Uses cascaded half-band filters for efficient decimation.
 * 
 * STATUS: WIP - Not yet tested. Awaiting modem team input on interface.
 */

#ifndef DECIMATOR_H
#define DECIMATOR_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Configuration
 *============================================================================*/

/**
 * Default decimation: 2,000,000 → 48,000 Hz
 * 
 * Strategy: 2M → 250k → 50k → 48k
 *   Stage 1: ÷8  (2M → 250k)
 *   Stage 2: ÷5  (250k → 50k)
 *   Stage 3: Polyphase 48/50 (50k → 48k)
 */

#define DECIM_INPUT_RATE    2000000.0
#define DECIM_OUTPUT_RATE   48000.0

/*============================================================================
 * Types
 *============================================================================*/

/** Complex sample (interleaved float) */
typedef struct {
    float i;
    float q;
} decim_complex_t;

/** Decimator context (opaque) */
typedef struct decim_state decim_state_t;

/** Error codes */
typedef enum {
    DECIM_OK = 0,
    DECIM_ERR_ALLOC,
    DECIM_ERR_INVALID_ARG,
    DECIM_ERR_BUFFER_FULL
} decim_error_t;

/*============================================================================
 * API
 *============================================================================*/

/**
 * @brief Get error description
 */
const char* decim_strerror(decim_error_t err);

/**
 * @brief Create decimator instance
 * 
 * @param state         Receives allocated state
 * @param input_rate    Input sample rate (e.g., 2000000)
 * @param output_rate   Output sample rate (e.g., 48000)
 * @return Error code
 */
decim_error_t decim_create(decim_state_t **state, 
                           double input_rate, 
                           double output_rate);

/**
 * @brief Destroy decimator and free resources
 * 
 * @param state  State to destroy (NULL safe)
 */
void decim_destroy(decim_state_t *state);

/**
 * @brief Reset decimator state (clear filter histories)
 * 
 * @param state  Decimator state
 */
void decim_reset(decim_state_t *state);

/**
 * @brief Process I/Q samples through decimator
 * 
 * Accepts int16 planar input (phoenix_sdr native format).
 * Outputs complex float at decimated rate.
 * 
 * @param state       Decimator state
 * @param xi          Input I samples (int16)
 * @param xq          Input Q samples (int16)
 * @param in_count    Number of input samples
 * @param out         Output buffer for complex samples
 * @param out_max     Maximum output samples (buffer size)
 * @param out_count   Receives actual output count
 * @return Error code
 */
decim_error_t decim_process_int16(
    decim_state_t *state,
    const int16_t *xi,
    const int16_t *xq,
    size_t in_count,
    decim_complex_t *out,
    size_t out_max,
    size_t *out_count
);

/**
 * @brief Process complex float samples through decimator
 * 
 * For use when input is already float (e.g., from file).
 * 
 * @param state       Decimator state
 * @param in          Input complex samples
 * @param in_count    Number of input samples
 * @param out         Output buffer for complex samples
 * @param out_max     Maximum output samples (buffer size)
 * @param out_count   Receives actual output count
 * @return Error code
 */
decim_error_t decim_process_float(
    decim_state_t *state,
    const decim_complex_t *in,
    size_t in_count,
    decim_complex_t *out,
    size_t out_max,
    size_t *out_count
);

/**
 * @brief Get decimation ratio
 * 
 * @param state  Decimator state
 * @return Decimation ratio (input_rate / output_rate)
 */
double decim_get_ratio(const decim_state_t *state);

/**
 * @brief Estimate output sample count for given input
 * 
 * @param state     Decimator state
 * @param in_count  Number of input samples
 * @return Approximate number of output samples
 */
size_t decim_estimate_output(const decim_state_t *state, size_t in_count);

/**
 * @brief Get actual output sample rate
 * 
 * May differ slightly from requested rate due to rational resampling.
 * 
 * @param state  Decimator state
 * @return Actual output rate in Hz
 */
double decim_get_output_rate(const decim_state_t *state);

#ifdef __cplusplus
}
#endif

#endif /* DECIMATOR_H */
