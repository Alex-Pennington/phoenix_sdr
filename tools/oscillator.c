/**
 * @file oscillator.c
 * @brief Phase-coherent sinusoidal oscillator implementation
 */

#include "oscillator.h"
#include <stdlib.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct oscillator {
    float sample_rate;      // Samples per second
    float frequency;        // Oscillator frequency in Hz
    float phase;            // Current phase in radians [0, 2π)
    float phase_increment;  // Phase advance per sample
    float amplitude;        // Amplitude multiplier [0, 1]
};

oscillator_t *oscillator_create(float sample_rate, float frequency) {
    if (sample_rate <= 0.0f || frequency < 0.0f) {
        return NULL;
    }

    oscillator_t *osc = (oscillator_t *)calloc(1, sizeof(oscillator_t));
    if (!osc) {
        return NULL;
    }

    osc->sample_rate = sample_rate;
    osc->frequency = frequency;
    osc->phase = 0.0f;
    osc->phase_increment = (2.0f * M_PI * frequency) / sample_rate;
    osc->amplitude = 1.0f;

    return osc;
}

void oscillator_destroy(oscillator_t *osc) {
    free(osc);
}

void oscillator_get_iq(oscillator_t *osc, float *i, float *q) {
    if (!osc || !i || !q) {
        return;
    }

    // Generate complex sample: I = cos(phase), Q = sin(phase)
    *i = osc->amplitude * cosf(osc->phase);
    *q = osc->amplitude * sinf(osc->phase);

    // Advance phase with wraparound at 2π
    osc->phase += osc->phase_increment;
    if (osc->phase >= 2.0f * M_PI) {
        osc->phase -= 2.0f * M_PI;
    }
}

void oscillator_set_amplitude(oscillator_t *osc, float amplitude) {
    if (!osc) {
        return;
    }
    osc->amplitude = amplitude;
}

float oscillator_get_amplitude(const oscillator_t *osc) {
    return osc ? osc->amplitude : 0.0f;
}

void oscillator_reset_phase(oscillator_t *osc) {
    if (!osc) {
        return;
    }
    osc->phase = 0.0f;
}

void oscillator_set_frequency(oscillator_t *osc, float frequency) {
    if (!osc || frequency < 0.0f) {
        return;
    }
    osc->frequency = frequency;
    osc->phase_increment = (2.0f * M_PI * frequency) / osc->sample_rate;
}
