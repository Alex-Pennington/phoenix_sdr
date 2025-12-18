#ifndef CHANNEL_FILTERS_H
#define CHANNEL_FILTERS_H

#ifdef __cplusplus
extern "C" {
#endif

// Biquad filter state (2nd-order IIR section)
typedef struct {
    float x1, x2;  // Input history
    float y1, y2;  // Output history
} biquad_state_t;

// Sync channel: 800-1400 Hz bandpass (4th order = 2 cascaded biquads)
typedef struct {
    biquad_state_t hp[2];  // 800 Hz highpass (2 sections)
    biquad_state_t lp[2];  // 1400 Hz lowpass (2 sections)
} sync_channel_t;

// Data channel: 0-150 Hz lowpass (4th order = 2 cascaded biquads)
typedef struct {
    biquad_state_t lp[2];  // 150 Hz lowpass (2 sections)
} data_channel_t;

// Initialize filters
void sync_channel_init(sync_channel_t *ch);
void data_channel_init(data_channel_t *ch);

// Process single sample through filter chain
float sync_channel_process(sync_channel_t *ch, float x);
float data_channel_process(data_channel_t *ch, float x);

// Reset filter state (call on reconnect)
void sync_channel_reset(sync_channel_t *ch);
void data_channel_reset(data_channel_t *ch);

#ifdef __cplusplus
}
#endif

#endif // CHANNEL_FILTERS_H
