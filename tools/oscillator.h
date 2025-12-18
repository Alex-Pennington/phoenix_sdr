/**
 * @file oscillator.h
 * @brief Phase-coherent sinusoidal oscillator for signal generation
 *
 * Provides continuous-phase tone generation with arbitrary frequency and amplitude.
 * Phase state persists across calls for seamless signal continuity.
 */

#ifndef OSCILLATOR_H
#define OSCILLATOR_H

#include <stdbool.h>

typedef struct oscillator oscillator_t;

/**
 * Create a new oscillator instance
 * @param sample_rate Sample rate in Hz (e.g., 2000000 for 2 Msps)
 * @param frequency Oscillator frequency in Hz
 * @return Allocated oscillator, or NULL on error
 */
oscillator_t *oscillator_create(float sample_rate, float frequency);

/**
 * Destroy oscillator and free resources
 * @param osc Oscillator instance
 */
void oscillator_destroy(oscillator_t *osc);

/**
 * Get next I/Q sample pair
 * @param osc Oscillator instance
 * @param i Output: in-phase component (normalized to ±1.0)
 * @param q Output: quadrature component (normalized to ±1.0)
 */
void oscillator_get_iq(oscillator_t *osc, float *i, float *q);

/**
 * Set oscillator amplitude
 * @param osc Oscillator instance
 * @param amplitude Amplitude multiplier (0.0 to 1.0)
 */
void oscillator_set_amplitude(oscillator_t *osc, float amplitude);

/**
 * Get current amplitude setting
 * @param osc Oscillator instance
 * @return Current amplitude multiplier
 */
float oscillator_get_amplitude(const oscillator_t *osc);

/**
 * Reset phase to zero (for test synchronization)
 * @param osc Oscillator instance
 */
void oscillator_reset_phase(oscillator_t *osc);

/**
 * Set oscillator frequency (can be changed dynamically)
 * @param osc Oscillator instance
 * @param frequency New frequency in Hz
 */
void oscillator_set_frequency(oscillator_t *osc, float frequency);

#endif // OSCILLATOR_H
