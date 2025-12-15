/**
 * @file waterfall_audio.h
 * @brief Audio output module for waterfall application
 *
 * Handles Windows waveOut audio output with volume and mute control.
 * This file CAN be modified - it's part of the audio path, not the frozen DSP.
 */

#ifndef WATERFALL_AUDIO_H
#define WATERFALL_AUDIO_H

#include <stdint.h>
#include <stdbool.h>

#define WF_AUDIO_SAMPLE_RATE   48000
#define WF_AUDIO_BUFFER_SIZE   1024

/* Initialize audio output system */
bool wf_audio_init(void);

/* Close audio output system */
void wf_audio_close(void);

/* Write samples to audio output */
void wf_audio_write(const int16_t *samples, uint32_t count);

/* Process a single sample through audio path and buffer it */
void wf_audio_process_sample(float sample);

/* Flush any remaining samples in the buffer */
void wf_audio_flush(void);

/* Volume control (1.0 - 500.0) */
float wf_audio_get_volume(void);
void wf_audio_set_volume(float volume);
void wf_audio_volume_up(void);
void wf_audio_volume_down(void);

/* Mute control */
bool wf_audio_is_enabled(void);
void wf_audio_set_enabled(bool enabled);
void wf_audio_toggle_mute(void);

#endif /* WATERFALL_AUDIO_H */
