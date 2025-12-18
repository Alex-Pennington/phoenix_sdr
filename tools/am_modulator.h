/**
 * @file am_modulator.h
 * @brief Amplitude modulation utilities for WWV signal generation
 *
 * Provides inline functions for AM modulation and multi-layer signal mixing.
 * No opaque type needed - pure utility functions.
 */

#ifndef AM_MODULATOR_H
#define AM_MODULATOR_H

/**
 * Apply AM modulation to a carrier signal
 * @param carrier_i In-phase carrier component
 * @param carrier_q Quadrature carrier component
 * @param mod_depth Modulation depth (0.0 to 1.0)
 * @param audio Audio signal to modulate with
 * @param out_i Output: modulated I component
 * @param out_q Output: modulated Q component
 */
static inline void am_mod_apply(float carrier_i, float carrier_q, float mod_depth,
                                float audio, float *out_i, float *out_q) {
    float envelope = 1.0f + (mod_depth * audio);
    *out_i = carrier_i * envelope;
    *out_q = carrier_q * envelope;
}

/**
 * Sum multiple modulation sources
 * @param depths Array of modulation depths for each source
 * @param signals Array of audio signals
 * @param count Number of sources
 * @return Combined modulation signal
 */
static inline float am_mod_sum_sources(const float *depths, const float *signals, int count) {
    float sum = 0.0f;
    for (int i = 0; i < count; i++) {
        sum += depths[i] * signals[i];
    }
    return sum;
}

/**
 * Clamp modulation depth to prevent over-modulation
 * @param mod_depth Total modulation depth
 * @return Clamped depth (0.0 to 1.0)
 */
static inline float am_mod_clamp(float mod_depth) {
    if (mod_depth > 1.0f) return 1.0f;
    if (mod_depth < 0.0f) return 0.0f;
    return mod_depth;
}

/**
 * Convert normalized float sample to int16_t
 * @param sample Float sample in range [-1.0, 1.0]
 * @return Scaled int16_t sample
 */
static inline short am_mod_float_to_int16(float sample) {
    if (sample > 1.0f) sample = 1.0f;
    if (sample < -1.0f) sample = -1.0f;
    return (short)(sample * 32767.0f);
}

#endif // AM_MODULATOR_H
