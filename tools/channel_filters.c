#include "channel_filters.h"
#include <string.h>

// Filter coefficients for fs=50000 Hz
// Format: [b0, b1, b2, a0(=1), a1, a2]

// Sync channel: 800 Hz Highpass (4th order = 2 cascaded 2nd-order sections)
static const float sync_hp_sos[2][6] = {
    // Section 1
    {0.94280904f, -1.88561808f, 0.94280904f, 1.0f, -1.88345806f, 0.88777810f},
    // Section 2
    {0.94280904f, -1.88561808f, 0.94280904f, 1.0f, -1.88345806f, 0.88777810f}
};

// Sync channel: 1400 Hz Lowpass (4th order = 2 cascaded 2nd-order sections)
static const float sync_lp_sos[2][6] = {
    // Section 1
    {0.00766530f, 0.01533060f, 0.00766530f, 1.0f, -1.73487628f, 0.76553747f},
    // Section 2
    {0.00766530f, 0.01533060f, 0.00766530f, 1.0f, -1.73487628f, 0.76553747f}
};

// Data channel: 150 Hz Lowpass (4th order = 2 cascaded 2nd-order sections)
static const float data_lp_sos[2][6] = {
    // Section 1
    {0.0000352f, 0.0000704f, 0.0000352f, 1.0f, -1.98223f, 0.98230f},
    // Section 2
    {0.0000352f, 0.0000704f, 0.0000352f, 1.0f, -1.98223f, 0.98230f}
};

// Process one sample through a biquad section
static float biquad_process(biquad_state_t *s, float x, const float *coeffs) {
    float y = coeffs[0] * x + coeffs[1] * s->x1 + coeffs[2] * s->x2
            - coeffs[4] * s->y1 - coeffs[5] * s->y2;
    s->x2 = s->x1;
    s->x1 = x;
    s->y2 = s->y1;
    s->y1 = y;
    return y;
}

// Initialize sync channel (800-1400 Hz bandpass)
void sync_channel_init(sync_channel_t *ch) {
    memset(ch, 0, sizeof(*ch));
}

// Initialize data channel (0-150 Hz lowpass)
void data_channel_init(data_channel_t *ch) {
    memset(ch, 0, sizeof(*ch));
}

// Process sample through sync channel (800-1400 Hz bandpass)
float sync_channel_process(sync_channel_t *ch, float x) {
    float y = x;

    // Highpass cascade (800 Hz)
    y = biquad_process(&ch->hp[0], y, sync_hp_sos[0]);
    y = biquad_process(&ch->hp[1], y, sync_hp_sos[1]);

    // Lowpass cascade (1400 Hz)
    y = biquad_process(&ch->lp[0], y, sync_lp_sos[0]);
    y = biquad_process(&ch->lp[1], y, sync_lp_sos[1]);

    return y;
}

// Process sample through data channel (0-150 Hz lowpass)
float data_channel_process(data_channel_t *ch, float x) {
    float y = x;

    // Lowpass cascade (150 Hz)
    y = biquad_process(&ch->lp[0], y, data_lp_sos[0]);
    y = biquad_process(&ch->lp[1], y, data_lp_sos[1]);

    return y;
}

// Reset sync channel state
void sync_channel_reset(sync_channel_t *ch) {
    memset(ch, 0, sizeof(*ch));
}

// Reset data channel state
void data_channel_reset(data_channel_t *ch) {
    memset(ch, 0, sizeof(*ch));
}
